#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "userprog/pipe.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "threads/malloc.h"

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

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

struct exec_sync
{
  char *cmdline;
  struct thread *parent;
  struct semaphore done;
  bool load_ok;
  struct pipe *stdin_pipe;
};

struct exec_args {
  char *file_name;
  struct pipe *stdin_pipe;
  struct exec_sync *es;
};

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
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

  es = malloc (sizeof *es);
  if (es == NULL)
    {
      palloc_free_page (fn_copy);
      palloc_free_page (tmp);
      return TID_ERROR;
    }
  es->cmdline = NULL;
  es->parent  = thread_current ();
  sema_init (&es->done, 0);
  es->load_ok = false;
  es->stdin_pipe = NULL;

  struct exec_args *args = malloc (sizeof *args);
  if (args == NULL)
    {
      palloc_free_page (fn_copy);
      palloc_free_page (tmp);
      free (es);
      return TID_ERROR;
    }
  args->file_name  = fn_copy;
  args->stdin_pipe = NULL;
  args->es         = es;

  {
    struct thread *cur = thread_current();
    int pfd = cur->pending_stdin_fd;
    if (pfd >= 0 && pfd < MAX_FD)
      {
        struct file *rf = cur->fd_table[pfd];
        if (rf && rf->file_type == FD_PIPE_READ && rf->pipe)
          {
            args->stdin_pipe = rf->pipe;
            es->stdin_pipe   = rf->pipe;
            pipe_ref_read_open (rf->pipe);
            cur->pending_stdin_fd = -1;
          }
      }
  }

  tid = thread_create (prog, PRI_DEFAULT, start_process, args);
  if (tid == TID_ERROR)
    {
      if (args->stdin_pipe)
        pipe_ref_read_close (args->stdin_pipe);
      free (args);
      palloc_free_page (fn_copy);
      palloc_free_page (tmp);
      free (es);
      return TID_ERROR;
    }

  cd = malloc (sizeof *cd);
  if (cd == NULL)
    {
      sema_down (&es->done);
      if (!es->load_ok && es->stdin_pipe)
        pipe_ref_read_close (es->stdin_pipe);
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

  sema_down (&es->done);
  if (!es->load_ok)
    {
      if (es->stdin_pipe)
        pipe_ref_read_close (es->stdin_pipe);
      palloc_free_page (tmp);
      free (es);
      return TID_ERROR;
    }

  palloc_free_page (tmp);
  free (es);
  return tid;
}

/* A thread function that loads a user process and starts it running. */
static void
start_process (void *args_)
{
  struct exec_args *args = (struct exec_args *) args_;
  struct exec_sync *es   = args->es;
  char *file_name        = args->file_name;
  struct intr_frame if_;
  bool success;
  char *ptr;
  char *arg;
  int arg_cnt;
  char *arg_list[32];

  struct thread *t = thread_current ();
  t->stdin_pipe = args->stdin_pipe;
  t->parent = es->parent;
  vm_init (&t->vm);

  arg_cnt = 0;
  for (arg = strtok_r (file_name, " ", &ptr);
       arg != NULL && arg_cnt < 32;
       arg = strtok_r (NULL, " ", &ptr))
    arg_list[arg_cnt++] = arg;

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;

  success = load (file_name, &if_.eip, &if_.esp);

  es->load_ok = success;
  sema_up (&es->done);

  if (!success)
    {
      palloc_free_page (file_name);
      free (args);
      thread_exit ();
      NOT_REACHED ();
    }

  argument_stack (arg_list, arg_cnt, &if_);

  palloc_free_page (file_name);
  free (args);

  asm volatile ("movl %0, %%esp; jmp intr_exit"
                :
                : "g" (&if_)
                : "memory");
  NOT_REACHED ();
}

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
    return -1; 
  if (cd->waited)
    return -1;
  cd->waited = true;

  if (!cd->exited)
    sema_down (&cd->sema);

  status = cd->exit_status;

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

  if (cur->exec_file != NULL) {
    file_allow_write(cur->exec_file);
    file_close(cur->exec_file);
    cur->exec_file = NULL;
  }

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

  vm_destroy(&cur->vm);

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
  /*
  while (read_bytes > 0 || zero_bytes > 0)
    {
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      struct vm_entry *vme = malloc (sizeof *vme);
      if (vme == NULL) return false;
      vme->upage      = upage;
      vme->writable   = writable;
      vme->type       = VM_ELF;
      vme->file       = file;
      vme->ofs        = ofs;
      vme->read_bytes = page_read_bytes;
      vme->zero_bytes = page_zero_bytes;
      vme->in_memory  = false;
      vme->swap_slot  = (size_t)-1;

      if (!insert_vme (&thread_current ()->vm, vme))
        { free (vme); return false; }

      ofs += page_read_bytes;
      upage += PGSIZE;
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
    }
  return true;  
  *///kws
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
 /*
  void *upage = ((uint8_t *) PHYS_BASE) - PGSIZE;
  struct vm_entry *vme = malloc (sizeof *vme);
  if (vme == NULL) return false;
  vme->upage      = upage;
  vme->writable   = true;
  vme->type       = VM_ANON;
  vme->file       = NULL;
  vme->ofs        = 0;
  vme->read_bytes = 0;
  vme->zero_bytes = PGSIZE;
  vme->in_memory  = false;
  vme->swap_slot  = (size_t)-1;
  if (!insert_vme (&thread_current ()->vm, vme))
    { free (vme); return false; }

  if (!handle_mm_fault (vme))
    return false;
  
  *esp = PHYS_BASE;
  return true;
  *///kws
}

static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}

bool
handle_mm_fault (struct vm_entry *vme)
{
  ASSERT (vme != NULL);
  ASSERT (!vme->in_memory);

  void *kpage = palloc_get_page (PAL_USER);
  if (kpage == NULL)
    return false;

  bool ok = false;
  switch (vme->type)
    {
    case VM_ELF:
      ok = load_file (kpage, vme);
      break;
    case VM_ANON:
      memset (kpage, 0, PGSIZE);
      ok = true;
      break;
    default:
      ok = false;
      break;
    }

  if (!ok)
    { palloc_free_page (kpage); return false; }

  if (!install_page (vme->upage, kpage, vme->writable))
    { palloc_free_page (kpage); return false; }

  vme->in_memory = true;
  return true;
}

bool
load_file (void *kaddr, struct vm_entry *vme)
{
  ASSERT (vme && vme->file);
  int bytes = file_read_at (vme->file, kaddr, vme->read_bytes, vme->ofs);
  if (bytes != (int)vme->read_bytes)
    return false;
  if (vme->zero_bytes)
    memset ((uint8_t *)kaddr + vme->read_bytes, 0, vme->zero_bytes);
  return true;
}