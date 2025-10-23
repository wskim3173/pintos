#include <syscall.h>
#include "tests/lib.h"
#include "tests/main.h"
#define NUM_ITER (1<<18) 
void
test_main(void)
{
  int fds[2];
  pid_t pid;
  char num[11] = "0123456789";
  
  CHECK(pipe(fds) >= 0, "open pipe");
  CHECK((pid = exec("child-long")) > 0, "exec child");

	close(fds[0]);
  int i;
  for(i = 0; i < NUM_ITER; i++)
  {
    if(write(fds[1], &(num[i%10]), 1)!=1)
    {
      exit(-1);
    }
  }
  close(fds[1]);
  wait(pid);
}
