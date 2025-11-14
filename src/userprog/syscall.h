#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <stdbool.h>
#include "userprog/pipe.h"
#include "threads/malloc.h"

typedef int pid_t;

void syscall_init (void);
void exit (int status);

#endif /* userprog/syscall.h */
