#include <syscall.h>
#include "tests/lib.h"
#include "tests/main.h"

char buf[50];

void test_main(void) {
  quiet = false;

  int fd;
  CHECK(create("data.txt", 100), "create data.txt with size 100");
  CHECK((fd = open("data.txt")) > 1, "open data.txt");
  CHECK(filesize(fd) == 100, "filesize equals initial size 100");
  close(fd);


  quiet = false;
}
