#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"
#include "filesys/filesys.h"
#include "userprog/process.h"
#include "threads/palloc.h"     /* copy_in_string에서 사용 */
#include "devices/input.h"        /* input_getc */
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

/* 한 바이트라도 접근 가능한지 및 유저영역인지 확인 */
static void
check_address (const void *addr)
{
  if (addr == NULL || !is_user_vaddr(addr) || get_user((const uint8_t *)addr) == -1)
    do_exit(-1);
}

/* 유저 메모리에서 커널 버퍼로 안전 복사 (바이트 단위, 페이지 경계 안전) */
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

/* 커널 버퍼 SRC의 데이터를 유저 주소 UDEST로 안전하게 복사한다.
   성공하면 true, 중간에 segfault 나면 false. */
static bool
copy_out (void *udst, const void *src, size_t n)
{
  uint8_t *up;
  const uint8_t *ks;
  size_t i;

  up = (uint8_t *)udst;
  ks = (const uint8_t *)src;

  for (i = 0; i < n; i++) {
    /* 유저 주소인지 확인 (PHYS_BASE 아래) */
    if (up == NULL || !is_user_vaddr(up))
      return false;

    /* 바이트 단위로 기록 시도: 실패하면 segfault로 간주 */
    if (!put_user(up, ks[i]))
      return false;

    up++;
  }
  return true;
}

/* 널 종료 문자열을 유저공간에서 커널 페이지 버퍼로 복사, 반환: palloc 페이지(호출자가 free) */
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

/* 주의: 커널은 유저용 exec() 래퍼를 부르지 않는다. process_execute 사용 */
static pid_t
do_exec (const char *u_file)
{
  char *kfile = copy_in_string(u_file);         /* 유저 문자열을 커널 버퍼로 */
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

/* 표준 C의 remove()와 충돌 피하기 위해 이름 변경 */
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
    copy_in(kpage, up, chunk);          // 유저→커널 안전 복사
    putbuf((const char *)kpage, chunk); // 콘솔로 출력
    up += chunk;
    remain -= (unsigned)chunk;
  }
  palloc_free_page(kpage);
  return (int)size;
}

/* fd 할당: 비어있는 슬롯 찾기 */
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

/* fd 유효성 체크 */
static struct file *fd_get(struct thread *t, int fd) {
  if (fd < 2 || fd >= MAX_FD) return NULL;
  return t->fd_table[fd];
}

/*
static void fd_close(struct thread *t, int fd) {
  if (fd >= 2 && fd < MAX_FD && t->fd_table[fd] != NULL) {
    lock_acquire(&filesys_lock);
    file_close(t->fd_table[fd]);
    lock_release(&filesys_lock);
    t->fd_table[fd] = NULL;
    if (fd < t->next_fd) t->next_fd = fd;
  }
}
*/

static void fd_close(struct thread *t, int fd) {
  if (fd >= 2 && fd < MAX_FD && t->fd_table[fd] != NULL) {
    struct file *f = t->fd_table[fd];

    /* 파이프인지, 일반파일인지 분기 */
    if (f->file_type == FD_PIPE_READ) {
      pipe_ref_read_close(f->pipe);   /* reader 감소/해제 */
      free(f);                        /* pipe_end_as_file()로 malloc 했으므로 free */
    } else if (f->file_type == FD_PIPE_WRITE) {
      pipe_ref_write_close(f->pipe);  /* writer 감소/해제 */
      free(f);                        /* 동일 */
    } else {
      /* 일반 파일은 기존처럼 filesys 락으로 보호해서 닫기 */
      lock_acquire(&filesys_lock);
      file_close(f);
      lock_release(&filesys_lock);
    }

    t->fd_table[fd] = NULL;
    if (fd < t->next_fd) t->next_fd = fd;
  }
}

/* read: fd==0(stdin) 특별 처리, 그 외 파일에서 읽기 */
static int sys_read(int fd, void *u_buf, unsigned size) {
  struct thread *cur = thread_current();
  unsigned remain;
  size_t chunk;
  void *kpage;
  int total = 0;

  if (size == 0) return 0;
  if (fd == 1) return -1;           /* stdout에 read 불가 */

  kpage = palloc_get_page(0);
  if (kpage == NULL) do_exit(-1);

  if (fd == 0) {
    /* 콘솔 입력: 바이트씩 받아서 user 버퍼로 복사 */
    remain = size;
    while (remain > 0) {
      uint8_t c = input_getc();
      if (!copy_out((uint8_t*)u_buf + total, &c, 1)) { /* 필요시 copy_out 구현 */
        palloc_free_page(kpage);
        do_exit(-1);
      }
      total++;
      remain--;
    }
    palloc_free_page(kpage);
    return total;
  } else {
    struct file *f = fd_get(cur, fd);
    int n;
    if (f == NULL) { palloc_free_page(kpage); return -1; }

    /* 🔹 파이프의 read end일 경우: pipe_read 호출 */
    if (f->file_type == FD_PIPE_READ) {
      palloc_free_page(kpage);
      return pipe_read(f->pipe, u_buf, size);
    }
    /* 🔹 파이프의 write end로 read 요청하면 -1 */
    if (f->file_type == FD_PIPE_WRITE) {
      palloc_free_page(kpage);
      return -1;
    }

    lock_acquire(&filesys_lock);
    remain = size;
    while (remain > 0) {
      chunk = remain > PGSIZE ? PGSIZE : remain;
      n = file_read(f, kpage, (off_t)chunk);
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

/* write: fd==1(stdout) or 파일 */
static int sys_write(int fd, const void *u_buf, unsigned size) {
  struct thread *cur = thread_current();
  if (fd == 0) return -1;

  /* 이미 stdout 전용 write_stdout이 있다면 그걸 호출해도 됨 */
  if (fd == 1) return write_stdout(u_buf, size);

  /* 파일로 쓰기 */
  {
    struct file *f = fd_get(cur, fd);
    void *kpage;
    unsigned remain;
    size_t chunk;
    int total = 0, n;

    if (f == NULL) return -1;
    if (size == 0) return 0;

    /* 🔹 파이프의 write end일 경우: pipe_write 호출 */
    if (f->file_type == FD_PIPE_WRITE) {
      return pipe_write(f->pipe, u_buf, size);
    }
    /* 🔹 파이프의 read end로 write 요청하면 -1 */
    if (f->file_type == FD_PIPE_READ) {
      return -1;
    }

    kpage = palloc_get_page(0);
    if (kpage == NULL) do_exit(-1);

    lock_acquire(&filesys_lock);
    remain = size;
    while (remain > 0) {
      chunk = remain > PGSIZE ? PGSIZE : remain;
      copy_in(kpage, (const uint8_t*)u_buf + total, chunk);
      n = file_write(f, kpage, (off_t)chunk);
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
    /* 두 끝 모두 닫으며 파이프 자원 회수 */
    pipe_ref_read_close(p);
    pipe_ref_write_close(p);
    return -1;
  }

  struct thread *cur = thread_current();
  int rfd = fd_alloc (cur, fr);
  int wfd = fd_alloc (cur, fw);

  if (rfd < 0 || wfd < 0) {
    if (rfd >= 0) fd_close(cur, rfd);  /* 파이프 전용 close 경로로 롤백 */
    if (wfd >= 0) fd_close(cur, wfd);
    else { /* fr만 열렸던 경우도 정리 */
      pipe_ref_read_close(p);
      free(fr);
    }
    return -1;
  }

  /* 유저 공간으로 fd 쌍 복사 */
  if (!copy_out (u_fds, &rfd, sizeof rfd) ||
      !copy_out (u_fds + 1, &wfd, sizeof wfd)) {
    /* 실패 시 두 FD 닫고 롤백 */
    fd_close(cur, rfd);
    fd_close(cur, wfd);
    return -1;
  }

  return 0;
}

/* ---------------- dispatcher ---------------- */

static void syscall_handler (struct intr_frame *f)
{
  void *u_esp = f->esp;
  check_address(u_esp);               /* sysno 읽기 전 스택 검증 */

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

    /* 파일 관련 나머지(read/write/open/close...)는 이후 단계에서 채움 */
    case SYS_OPEN:   { const char *n = (const char*)get_ptr_arg(u_esp,0); f->eax = sys_open(n); break; }
    case SYS_CLOSE:  { int fd = get_int_arg(u_esp,0); sys_close(fd); break; }
    case SYS_READ:   { int fd = get_int_arg(u_esp,0); void*buf=(void*)get_ptr_arg(u_esp,1); unsigned sz=(unsigned)get_int_arg(u_esp,2); f->eax = sys_read(fd,buf,sz); break; }
    case SYS_WRITE:  { int fd = get_int_arg(u_esp,0); const void*buf=(const void*)get_ptr_arg(u_esp,1); unsigned sz=(unsigned)get_int_arg(u_esp,2); f->eax = sys_write(fd,buf,sz); break; }
    case SYS_FILESIZE:{ int fd = get_int_arg(u_esp,0); f->eax = sys_filesize(fd); break; }
    case SYS_SEEK:   { int fd = get_int_arg(u_esp,0); unsigned pos=(unsigned)get_int_arg(u_esp,1); sys_seek(fd,pos); break; }
    case SYS_TELL:   { int fd = get_int_arg(u_esp,0); f->eax = sys_tell(fd); break; }
    case SYS_PIPE: {
      int *u_fds = (int *) get_ptr_arg(u_esp, 0);
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
