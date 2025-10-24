#include <syscall.h>
#include "tests/lib.h"
#include "tests/main.h"

void
test_main(void)
{
  int fds[2];
  
  CHECK(pipe(fds) >= 0, "open pipe");
  close(fds[0]);
  close(fds[1]);
}
