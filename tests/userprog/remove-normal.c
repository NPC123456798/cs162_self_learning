#include <syscall.h>
#include "tests/lib.h"
#include "tests/main.h"

void test_main(void) {
  quiet = false;

  CHECK(create("temp.txt", 100), "create temp.txt");
  CHECK(remove("temp.txt"), "remove temp.txt");
  CHECK(!remove("temp.txt"), "remove nonexistent temp.txt");
  int fd = open("temp.txt");
  CHECK(fd == -1, "open removed temp.txt");

  quiet = false;
}
