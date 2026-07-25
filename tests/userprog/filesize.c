#include <syscall.h>
#include "tests/lib.h"
#include "tests/main.h"

char buf[50];

void test_main(void) {
  quiet = false;

  int fd;
  CHECK(create("data.txt", 100), "create data.txt");
  CHECK((fd = open("data.txt")) > 1, "open data.txt");
  CHECK(filesize(fd) == 100, "filesize is 100 at creation");

  // write(fd, "hello", 5);
  // CHECK(filesize(fd) == 5, "filesize after write");

  // seek(fd, 100);
  // write(fd, "x", 1);
  // CHECK(filesize(fd) == 101, "filesize after extension");
  close(fd);

  quiet = false;
}
