#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "threads/interrupt.h"
#include "vm/page.h"

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);
void argument_stack (char **argv, int argc, struct intr_frame *if_);

bool handle_mm_fault (struct vm_entry *vme);
bool load_file (void *kaddr, struct vm_entry *vme);

#endif /* userprog/process.h */
