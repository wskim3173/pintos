#include <syscall.h>
#include "tests/lib.h"
#include "tests/main.h"

void
test_main(void)
{
  int fds[2];
  pid_t pid;
  char send_msg[10] = "Happy OS!";
  
  CHECK(pipe(fds) >= 0, "open pipe");
  CHECK((pid = exec("child-short")) > 0, "exec child");

	close(fds[0]);
  if(write(fds[1], send_msg, 10) != 10)
  {
    exit(-1);
  }
  close(fds[1]);
  wait(pid);
}
