#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"
#include "filesys/filesys.h"
#include "userprog/process.h"
#include "threads/palloc.h"
#include "devices/input.h"
#include "filesys/file.h"

static struct lock filesys_lock;

/* ---------------- Safe user-memory primitives (x86, kernel only) ---------------- */

/* Reads a byte at user virtual address UADDR.
   Returns the byte value 0..255 on success, or -1 on fault. */
static int
get_user (const uint8_t *uaddr)
{
  int result;
  asm ("movl $1f, %0; movzbl %1, %0; 1:"
       : "=&a" (result) : "m" (*uaddr));
  return result;
}

/* Writes BYTE to user address UDST.
   Returns true on success, false on fault. */
static bool
put_user (uint8_t *udst, uint8_t byte)
{
  int error_code;
  asm ("movl $1f, %0; movb %b2, %1; 1:"
       : "=&a" (error_code), "=m" (*udst) : "q" (byte));
  return error_code != -1;
}

/* ---- helpers: validation & safe copy from user ---- */

static void NO_RETURN do_exit(int status);

static void
check_address (const void *addr)
{
  if (addr == NULL || !is_user_vaddr(addr) || get_user((const uint8_t *)addr) == -1)
    do_exit(-1);
}

static void
copy_in (void *dst, const void *usrc, size_t n)
{
  uint8_t *kd = (uint8_t *)dst;
  const uint8_t *up = (const uint8_t *)usrc;
  size_t i;

  for (i = 0; i < n; i++) {
    if (up == NULL || !is_user_vaddr(up))
      do_exit(-1);

    int ch = get_user(up);
    if (ch == -1) do_exit(-1);
    *kd = (uint8_t)ch;
    up++;
    kd++;
  }
}

static bool
copy_out (void *udst, const void *src, size_t n)
{
  uint8_t *up;
  const uint8_t *ks;
  size_t i;

  up = (uint8_t *)udst;
  ks = (const uint8_t *)src;

  for (i = 0; i < n; i++) {
    if (up == NULL || !is_user_vaddr(up))
      return false;

    if (!put_user(up, ks[i]))
      return false;

    up++;
  }
  return true;
}

#define MAX_STR_LEN PGSIZE
static char *
copy_in_string (const char *u_str)
{
  char *kbuf;
  size_t i;
  int ch;

  kbuf = palloc_get_page(0);
  if (kbuf == NULL) do_exit(-1);

  i = 0;
  for (;;) {
    ch = get_user((const uint8_t *)u_str);
    if (ch == -1) { palloc_free_page(kbuf); do_exit(-1); }
    if (i >= MAX_STR_LEN - 1) { palloc_free_page(kbuf); do_exit(-1); }
    kbuf[i] = (char)ch;
    i++;
    if (ch == '\0') break;
    u_str++;
  }
  return kbuf;
}
/* ---- stack-ABI arg fetchers (push args; push sysno; int $0x30) ---- */
/* u_esp: [sysno][arg1][arg2]...  */
static int
get_sysno (const void *u_esp)
{
  int n;
  copy_in(&n, u_esp, sizeof n);
  return n;
}

static int
get_int_arg (const void *u_esp, int idx)
{
  int val;
  const uint8_t *slot;
  slot = (const uint8_t *)u_esp + 4 * (idx + 1);
  copy_in(&val, slot, sizeof val);
  return val;
}

static void *
get_ptr_arg (const void *u_esp, int idx)
{
  void *p;
  const uint8_t *slot;
  slot = (const uint8_t *)u_esp + 4 * (idx + 1);
  copy_in(&p, slot, sizeof p);
  if (p != NULL) check_address(p);
  return p;
}

/* ---------------- syscalls (kernel side) ---------------- */

static void NO_RETURN
do_halt (void)
{
  shutdown_power_off();
  NOT_REACHED();
}

static void NO_RETURN
do_exit (int status)
{
  struct thread *t = thread_current();
  t->exit_status = status;
  printf("%s: exit(%d)\n", t->name, status);
  thread_exit();
  NOT_REACHED();
}

void
exit (int status)
{
  do_exit(status);
}

static pid_t
do_exec (const char *u_file)
{
  char *kfile = copy_in_string(u_file);
  pid_t pid = process_execute(kfile);
  palloc_free_page(kfile);
  return pid;
}

static int
do_wait (pid_t pid)
{
  return process_wait(pid);
}

static bool
sys_create (const char *u_name, unsigned initial_size)
{
  char *kname = copy_in_string(u_name);
  bool ok = filesys_create(kname, initial_size);
  palloc_free_page(kname);
  return ok;
}

static bool
sys_remove (const char *u_name)
{
  char *kname = copy_in_string(u_name);
  bool ok = filesys_remove(kname);
  palloc_free_page(kname);
  return ok;
}

static int
write_stdout (const void *u_buf, unsigned size)
{
  void *kpage;
  const uint8_t *up;
  size_t chunk;
  unsigned remain;

  if (size == 0) return 0;
  kpage = palloc_get_page(0);
  if (kpage == NULL) do_exit(-1);

  up = (const uint8_t *)u_buf;
  remain = size;
  while (remain > 0) {
    chunk = remain > PGSIZE ? PGSIZE : remain;
    copy_in(kpage, up, chunk);
    putbuf((const char *)kpage, chunk);
    up += chunk;
    remain -= (unsigned)chunk;
  }
  palloc_free_page(kpage);
  return (int)size;
}

static int fd_alloc(struct thread *t, struct file *f) {
  int i = t->next_fd, start = t->next_fd;
  if (f == NULL) return -1;
  for (;;) {
    if (i >= MAX_FD) i = 2;
    if (t->fd_table[i] == NULL) { t->fd_table[i] = f; t->next_fd = i + 1; return i; }
    i++;
    if (i == start) return -1;
  }
}

static struct file *fd_get(struct thread *t, int fd) {
  if (fd < 2 || fd >= MAX_FD) return NULL;
  return t->fd_table[fd];
}

static void fd_close(struct thread *t, int fd) {
  if (fd >= 2 && fd < MAX_FD && t->fd_table[fd] != NULL) {
    struct file *f = t->fd_table[fd];

    if (f->file_type == FD_PIPE_READ) {
      pipe_ref_read_close(f->pipe);
      free(f);
    } else if (f->file_type == FD_PIPE_WRITE) {
      pipe_ref_write_close(f->pipe);
      free(f);
    } else {
      lock_acquire(&filesys_lock);
      file_close(f);
      lock_release(&filesys_lock);
    }

    t->fd_table[fd] = NULL;
    if (fd < t->next_fd) t->next_fd = fd;
  }
}

static int sys_read(int fd, void *u_buf, unsigned size) {
  struct thread *cur = thread_current();
  if (size == 0) return 0;
  if (fd == 1) return -1;

  if (fd == 0) {
    if (cur->stdin_pipe) {
      void *kpage = palloc_get_page(0);
      if (!kpage) do_exit(-1);

      unsigned remain = size; int total = 0;
      while (remain > 0) {
        size_t chunk = remain > PGSIZE ? PGSIZE : remain;
        int n = pipe_read(cur->stdin_pipe, kpage, chunk);
        if (n <= 0) break;
        if (!copy_out((uint8_t*)u_buf + total, kpage, (size_t)n)) {
          palloc_free_page(kpage); do_exit(-1);
        }
        total += n; remain -= (unsigned)n;
        if ((size_t)n < chunk) break;
      }
      palloc_free_page(kpage);
      return total;
    } else {
      unsigned remain = size; int total = 0;
      while (remain--) {
        uint8_t c = input_getc();
        if (!copy_out((uint8_t*)u_buf + total, &c, 1)) do_exit(-1);
        total++;
      }
      return total;
    }
  }

  struct file *f = fd_get(cur, fd);
  if (!f) return -1;

  if (f->file_type == FD_PIPE_READ) {
    void *kpage = palloc_get_page(0);
    if (!kpage) do_exit(-1);

    unsigned remain = size;
    int total = 0;
    while (remain > 0) {
      size_t chunk = remain > PGSIZE ? PGSIZE : remain;
      int n = pipe_read(f->pipe, kpage, chunk);
      if (n <= 0) break;
      if (!copy_out((uint8_t*)u_buf + total, kpage, (size_t)n)) {
        palloc_free_page(kpage);
        do_exit(-1);
      }
      total += n;
      remain -= (unsigned)n;
      if ((size_t)n < chunk) break;
    }
    palloc_free_page(kpage);
    return total;
  }

  if (f->file_type == FD_PIPE_WRITE)
    return -1;

  {
    void *kpage = palloc_get_page(0);
    if (!kpage) do_exit(-1);

    unsigned remain = size;
    int total = 0;
    lock_acquire(&filesys_lock);
    while (remain > 0) {
      size_t chunk = remain > PGSIZE ? PGSIZE : remain;
      int n = file_read(f, kpage, (off_t)chunk);
      if (n < 0) { total = -1; break; }
      if (n == 0) break;
      if (!copy_out((uint8_t*)u_buf + total, kpage, (size_t)n)) {
        lock_release(&filesys_lock);
        palloc_free_page(kpage);
        do_exit(-1);
      }
      total += n;
      remain -= (unsigned)n;
      if ((size_t)n < chunk) break;
    }
    lock_release(&filesys_lock);
    palloc_free_page(kpage);
    return total;
  }
}

static int sys_write(int fd, const void *u_buf, unsigned size) {
  struct thread *cur = thread_current();
  if (fd == 0) return -1; 
  if (size == 0) return 0;
  if (fd == 1) return write_stdout(u_buf, size);

  struct file *f = fd_get(cur, fd);
  if (!f) return -1;

  if (f->file_type == FD_PIPE_WRITE) {
    void *kpage = palloc_get_page(0);
    if (!kpage) do_exit(-1);

    unsigned remain = size;
    int total = 0;
    while (remain > 0) {
      size_t chunk = remain > PGSIZE ? PGSIZE : remain;
      copy_in(kpage, (const uint8_t*)u_buf + total, chunk);
      int n = pipe_write(f->pipe, kpage, chunk);
      if (n < 0) {
        total = -1;
        break;
      }
      total += n;
      remain -= (unsigned)n;
      if ((size_t)n < chunk) break;
    }
    palloc_free_page(kpage);
    return total;
  }

  if (f->file_type == FD_PIPE_READ)
    return -1;

  {
    void *kpage = palloc_get_page(0);
    if (!kpage) do_exit(-1);

    unsigned remain = size;
    int total = 0;
    lock_acquire(&filesys_lock);
    while (remain > 0) {
      size_t chunk = remain > PGSIZE ? PGSIZE : remain;
      copy_in(kpage, (const uint8_t*)u_buf + total, chunk);
      int n = file_write(f, kpage, (off_t)chunk);
      if (n <= 0) { total = (n < 0 ? -1 : total); break; }
      total += n;
      remain -= (unsigned)n;
      if ((size_t)n < chunk) break;
    }
    lock_release(&filesys_lock);
    palloc_free_page(kpage);
    return total;
  }
}

/* open/close/filesize/seek/tell */
static int sys_open(const char *u_name) {
  char *kname;
  struct file *f;
  int fd;

  kname = copy_in_string(u_name);
  lock_acquire(&filesys_lock);
  f = filesys_open(kname);
  lock_release(&filesys_lock);
  palloc_free_page(kname);

  if (f == NULL) return -1;
  fd = fd_alloc(thread_current(), f);
  if (fd < 0) { lock_acquire(&filesys_lock); file_close(f); lock_release(&filesys_lock); }
  return fd;
}

static void sys_close(int fd) { fd_close(thread_current(), fd); }

static int sys_filesize(int fd) {
  struct file *f = fd_get(thread_current(), fd);
  int sz;
  if (f == NULL) return -1;
  lock_acquire(&filesys_lock);
  sz = file_length(f);
  lock_release(&filesys_lock);
  return sz;
}

static void sys_seek(int fd, unsigned pos) {
  struct file *f = fd_get(thread_current(), fd);
  if (f == NULL) return;
  lock_acquire(&filesys_lock);
  file_seek(f, (off_t)pos);
  lock_release(&filesys_lock);
}

static unsigned sys_tell(int fd) {
  struct file *f = fd_get(thread_current(), fd);
  unsigned p = 0;
  if (f == NULL) return 0;
  lock_acquire(&filesys_lock);
  p = (unsigned)file_tell(f);
  lock_release(&filesys_lock);
  return p;
}

static struct file *
pipe_end_as_file (struct pipe *p, bool is_reader)
{
  struct file *f = malloc (sizeof *f);
  if (!f) return NULL;
  f->inode = NULL;
  f->pos = 0;
  f->deny_write = false;
  f->pipe = p;
  f->file_type = is_reader ? FD_PIPE_READ : FD_PIPE_WRITE;
  return f;
}

static int
sys_pipe (int *u_fds)
{
  if (!u_fds) return -1;

  struct pipe *p = pipe_create ();
  if (!p) return -1;

  struct file *fr = pipe_end_as_file (p, true);
  struct file *fw = pipe_end_as_file (p, false);
  if (!fr || !fw) {
    free(fr); free(fw);
    pipe_ref_read_close(p);
    pipe_ref_write_close(p);
    return -1;
  }

  struct thread *cur = thread_current();
  int rfd = fd_alloc (cur, fr);
  int wfd = fd_alloc (cur, fw);

  if (rfd < 0 || wfd < 0) {
    if (rfd >= 0) fd_close(cur, rfd);
    if (wfd >= 0) fd_close(cur, wfd);
    else {
      pipe_ref_read_close(p);
      free(fr);
    }
    return -1;
  }

  if (!copy_out (u_fds, &rfd, sizeof rfd) ||
      !copy_out (u_fds + 1, &wfd, sizeof wfd)) {
    fd_close(cur, rfd);
    fd_close(cur, wfd);
    return -1;
  }

  thread_current()->pending_stdin_fd = rfd;

  return 0;
}

/* ---------------- dispatcher ---------------- */

static void syscall_handler (struct intr_frame *f)
{
  void *u_esp = f->esp;
  check_address(u_esp);

  int sysno = get_sysno(u_esp);

  switch (sysno) {
    case SYS_HALT:
      do_halt();                      /* NO_RETURN */

    case SYS_EXIT: {
      int status = get_int_arg(u_esp, 0);
      do_exit(status);                /* NO_RETURN */
      break;
    }

    case SYS_EXEC: {
      const char *u_file = (const char *)get_ptr_arg(u_esp, 0);
      f->eax = do_exec(u_file);
      break;
    }

    case SYS_WAIT: {
      pid_t pid = (pid_t)get_int_arg(u_esp, 0);
      f->eax = do_wait(pid);
      break;
    }

    case SYS_CREATE: {
      const char *u_name = (const char *)get_ptr_arg(u_esp, 0);
      unsigned sz = (unsigned)get_int_arg(u_esp, 1);
      f->eax = sys_create(u_name, sz);
      break;
    }

    case SYS_REMOVE: {
      const char *u_name = (const char *)get_ptr_arg(u_esp, 0);
      f->eax = sys_remove(u_name);
      break;
    }

    case SYS_OPEN:
    {
      const char *n = (const char *)get_ptr_arg(u_esp, 0);
      f->eax = sys_open(n);
      break;
    }
    case SYS_CLOSE:
    {
      int fd = get_int_arg(u_esp, 0);
      sys_close(fd);
      break;
    }
    case SYS_READ:
    {
      int fd = get_int_arg(u_esp, 0);
      void *buf = (void *)get_ptr_arg(u_esp, 1);
      unsigned sz = (unsigned)get_int_arg(u_esp, 2);
      f->eax = sys_read(fd, buf, sz);
      break;
    }
    case SYS_WRITE:
    {
      int fd = get_int_arg(u_esp, 0);
      const void *buf = (const void *)get_ptr_arg(u_esp, 1);
      unsigned sz = (unsigned)get_int_arg(u_esp, 2);
      f->eax = sys_write(fd, buf, sz);
      break;
    }
    case SYS_FILESIZE:
    {
      int fd = get_int_arg(u_esp, 0);
      f->eax = sys_filesize(fd);
      break;
    }
    case SYS_SEEK:
    {
      int fd = get_int_arg(u_esp, 0);
      unsigned pos = (unsigned)get_int_arg(u_esp, 1);
      sys_seek(fd, pos);
      break;
    }
    case SYS_TELL:
    {
      int fd = get_int_arg(u_esp, 0);
      f->eax = sys_tell(fd);
      break;
    }
    case SYS_PIPE:
    {
      int *u_fds = (int *)get_ptr_arg(u_esp, 0);
      f->eax = sys_pipe(u_fds);
      break;
    }
    default:
      do_exit(-1);
  }
}

/* ---------------- public init ---------------- */

void
syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init(&filesys_lock);  
}
