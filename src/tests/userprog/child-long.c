/* Child process run by exec-multiple, exec-one, wait-simple, and
   wait-twice tests.
   Just prints a single message and terminates. */

#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include "tests/lib.h"
#define NUM_ITER (1<< 18)

int main(void) {
    test_name = "child-long";
    char true_str[11] = "0123456789";
    char buf;
    int i = 0;
    while(read(0, &buf, 1) > 0)
    {
      if(buf != true_str[i%10])
      {
        exit(-1);
      }
      i++;
    }

    return 0;
}

