/* Child process run by exec-multiple, exec-one, wait-simple, and
   wait-twice tests.
   Just prints a single message and terminates. */

#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include "tests/lib.h"

int main(void) {
    test_name = "child-short";
    char true_str[10] = "Happy OS!";
    char recv_str[10];
    char buf;
    int i = 0;
    while(read(0, &buf, 1) > 0)
    {
      recv_str[i] = buf;
      i++;
    }
    CHECK(strcmp(true_str, recv_str) == 0, "received a msg");

    return 0;
}

