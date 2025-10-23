#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

/* 추가: 부모-자식 동기화/리스트 관리에 필요 */
#include "threads/synch.h"
#include "threads/malloc.h"

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

/* ----------- 추가: 도우미들 ----------- */

/* 부모의 children 리스트에서 child_tid 찾기 */
static struct child_desc *
find_child_desc (struct thread *parent, tid_t child_tid)
{
  struct list_elem *e;
  for (e = list_begin (&parent->children);
       e != list_end (&parent->children);
       e = list_next (e))
    {
      struct child_desc *cd = list_entry (e, struct child_desc, elem);
      if (cd->tid == child_tid)
        return cd;
    }
  return NULL;
}

/* exec 로딩 동기화용 패키지(부모->자식 전달) */
struct exec_sync
{
  char *cmdline;             /* palloc 페이지에 복사된 전체 커맨드 라인 */
  struct thread *parent;     /* 부모 스레드 포인터 */
  struct semaphore done;     /* 자식 로딩 완료 신호 */
  bool load_ok;              /* 로딩 성공 여부 */
};

/* ----------- 기존 주석 유지 ----------- */

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  /* 변경 요약:
     - 로딩 완료까지 부모가 기다리도록 exec_sync 사용
     - 부모의 children 리스트에 child_desc 등록
     - 프로그램 이름 분리를 위해 임시 버퍼는 사용 후 해제 */
  char *fn_copy;
  char *tmp;
  tid_t tid;
  char *save_ptr;
  char *prog;
  struct exec_sync *es;
  struct child_desc *cd;

  /* Make a copy of FILE_NAME. Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  tmp = palloc_get_page (0);
  if (tmp == NULL)
    {
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }
  strlcpy (tmp, file_name, PGSIZE);

  prog = strtok_r (tmp, " ", &save_ptr);

  /* exec_sync 준비 */
  es = malloc (sizeof *es);
  if (es == NULL)
    {
      palloc_free_page (fn_copy);
      palloc_free_page (tmp);
      return TID_ERROR;
    }
  es->cmdline = fn_copy;                 /* 자식이 사용할 커맨드 복사본 */
  es->parent  = thread_current ();
  sema_init (&es->done, 0);
  es->load_ok = false;

  /* Create a new thread to execute FILE_NAME. (스레드 이름은 prog로) */
  tid = thread_create (prog, PRI_DEFAULT, start_process, es);
  if (tid == TID_ERROR)
    {
      palloc_free_page (fn_copy);
      palloc_free_page (tmp);
      free (es);
      return TID_ERROR;
    }

  /* 부모에 child_desc 등록 (wait 용) */
  cd = malloc (sizeof *cd);
  if (cd == NULL)
    {
      /* 메모리 부족이어도 자식 로딩 완료까지는 기다려서 일관성 유지 */
      sema_down (&es->done);
      palloc_free_page (tmp);
      free (es);
      return TID_ERROR;
    }
  cd->tid = tid;
  cd->exit_status = -1;
  cd->exited = false;
  cd->waited = false;
  sema_init (&cd->sema, 0);
  list_push_back (&thread_current ()->children, &cd->elem);

  /* 자식 로딩 완료까지 대기 */
  sema_down (&es->done);
  if (!es->load_ok)
    {
      /* 로딩 실패 */
      palloc_free_page (tmp);
      free (es);
      return TID_ERROR;
    }

  /* 임시 버퍼/패키지 정리 */
  palloc_free_page (tmp);
  free (es);
  return tid;
}

/* A thread function that loads a user process and starts it running. */
static void
start_process (void *es_)
{
  /* 변경: 인자로 exec_sync* 를 받는다 */
  struct exec_sync *es = (struct exec_sync *) es_;
  char *file_name = es->cmdline;     /* 부모가 넘겨준 커맨드 라인 */
  struct intr_frame if_;
  bool success;
  char *ptr;
  char *arg;
  int arg_cnt;
  char *arg_list[32];

  /* 현재 스레드의 부모 포인터 설정 */
  thread_current ()->parent = es->parent;

  arg_cnt = 0;
  for (arg = strtok_r (file_name, " ", &ptr); arg != NULL; arg = strtok_r (NULL, " ", &ptr))
    {
      if (arg_cnt < 32)
        arg_list[arg_cnt++] = arg;
    }

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;

  success = load (file_name, &if_.eip, &if_.esp);

  /* 부모에게 로딩 결과 알림 (세마 up) */
  es->load_ok = success;
  sema_up (&es->done);

  if (!success)
    {
      /* 커맨드 버퍼는 부모가 가진 페이지이지만, 관례상 여기서도 안전하게 free */
      palloc_free_page (file_name);
      thread_exit ();
      NOT_REACHED ();
    }

  /* 인자 스택 구축 */
  argument_stack (arg_list, arg_cnt, &if_);

  /* 커맨드 버퍼 해제 */
  palloc_free_page (file_name);

  /* (디버깅용) 스택 덤프 */
  /* hex_dump ((uintptr_t)if_.esp, (const void *)if_.esp,
              (size_t)((uintptr_t)PHYS_BASE - (uintptr_t)if_.esp), true); */

  /* intr_exit로 복귀 */
  asm volatile ("movl %0, %%esp; jmp intr_exit"
                :
                : "g" (&if_)
                : "memory");
  NOT_REACHED ();
}

/* 인자 스택 구축 (네 코드 그대로 사용) */
void
argument_stack (char **argv, int argc, struct intr_frame *if_)
{
  enum { MAX_ARGC = 128 };
  uint8_t *sp;
  char *arg_addr[MAX_ARGC];
  int i;

  if (argc < 0 || argc > MAX_ARGC) return;

  sp = (uint8_t *) if_->esp;

  for (i = argc - 1; i >= 0; --i)
    {
      size_t len = strlen (argv[i]) + 1;
      sp -= len;
      memcpy (sp, argv[i], len);
      arg_addr[i] = (char *) sp;
    }

  /* align to 4 */
  sp = (uint8_t *) (((uintptr_t) sp) & ~((uintptr_t) 3));

  /* argv[argc] = NULL */
  sp -= sizeof (char *);
  *(char **) sp = NULL;

  for (i = argc - 1; i >= 0; --i)
    {
      sp -= sizeof (char *);
      *(char **) sp = arg_addr[i];
    }

  /* user_argv */
  {
    char **user_argv = (char **) sp;
    sp -= sizeof (char **);
    *(char ***) sp = user_argv;  /* argv */
  }

  /* argc */
  sp -= sizeof (int);
  *(int *) sp = argc;

  /* fake return address */
  sp -= sizeof (void *);
  *(void **) sp = NULL;

  if_->esp = (void *) sp;
}

/* Waits for thread TID to die and returns its exit status. */
int
process_wait (tid_t child_tid) 
{
  struct thread *cur;
  struct child_desc *cd;
  int status;

  cur = thread_current ();
  cd = find_child_desc (cur, child_tid);
  if (cd == NULL)
    return -1;               /* 직계 자식 아님 */
  if (cd->waited)
    return -1;               /* 이미 wait 했음 */
  cd->waited = true;

  if (!cd->exited)
    sema_down (&cd->sema);   /* 자식 종료 대기 */

  status = cd->exit_status;

  /* 디스크립터 회수 */
  list_remove (&cd->elem);
  free (cd);

  return status;
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur;
  uint32_t *pd;

  cur = thread_current ();

  int i;
  for (i = 2; i < MAX_FD; i++) {
    if (cur->fd_table[i] != NULL) {
      file_close(cur->fd_table[i]);
      cur->fd_table[i] = NULL;
    }
  }  

  /* 실행 파일 쓰기 금지 해제 및 닫기 */
  if (cur->exec_file != NULL) {
    file_allow_write(cur->exec_file);
    file_close(cur->exec_file);
    cur->exec_file = NULL;
  }

  /* 부모에게 종료 상태 전달 + 깨우기 */
  if (cur->parent != NULL)
    {
      struct child_desc *cd = find_child_desc (cur->parent, cur->tid);
      if (cd != NULL)
        {
          cd->exit_status = cur->exit_status;
          cd->exited = true;
          sema_up (&cd->sema);
        }
    }

  /* Destroy the current process's page directory and switch back to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current thread. */
void
process_activate (void)
{
  struct thread *t = thread_current ();
  pagedir_activate (t->pagedir);
  tss_update ();
}

/* ---- ELF 로더 코드(기존 유지) ---- */

typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

#define PE32Wx PRIx32
#define PE32Ax PRIx32
#define PE32Ox PRIx32
#define PE32Hx PRIx16

struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_SHLIB   5
#define PT_PHDR    6
#define PT_STACK   0x6474e551

#define PF_X 1
#define PF_W 2
#define PF_R 4

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable … (기존 구현 유지) */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  file = filesys_open (file_name);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }

  t->exec_file = file;
  file_deny_write(file);
  
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  if (!setup_stack (esp))
    goto done;

  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  if (!success && file != NULL) {
    /* 실패 시에만 닫기 */
    file_close(file);
    t->exec_file = NULL;
  }
  return success;
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  if (phdr->p_memsz == 0)
    return false;
  
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  if (phdr->p_vaddr < PGSIZE)
    return false;

  return true;
}

static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  uint8_t *kpage;
  size_t page_read_bytes;
  size_t page_zero_bytes;

  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      page_zero_bytes = PGSIZE - page_read_bytes;

      kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false; 
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      if (!install_page (upage, kpage, writable)) 
        {
          palloc_free_page (kpage);
          return false; 
        }

      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

static bool
setup_stack (void **esp) 
{
  uint8_t *kpage;
  bool success;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  success = false;
  if (kpage != NULL) 
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        *esp = PHYS_BASE;
      else
        palloc_free_page (kpage);
    }
  return success;
}

static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
