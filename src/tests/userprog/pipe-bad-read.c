#include <syscall.h>
#include "tests/lib.h"
#include "tests/main.h"

void
test_main(void)
{
  int fds[2];
  char buf = 123;
  
  CHECK(pipe(fds) >= 0, "open pipe");
  CHECK(read (fds[1], &buf, 1) < 0, "read prohibited");
}
