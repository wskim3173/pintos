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
    int ch = get_user(up);
    if (ch == -1) do_exit(-1);
    *kd = (uint8_t)ch;
    up++;
    kd++;
  }
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

    default:
      do_exit(-1);
  }
}

/* ---------------- public init ---------------- */

void
syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}
