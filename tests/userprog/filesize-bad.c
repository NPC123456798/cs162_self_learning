#include <syscall.h>
#include "tests/lib.h"
#include "tests/main.h"

void test_main(void) {
  quiet = false;

  CHECK(filesize(-1) == -1, "filesize of -1");
  CHECK(filesize(5) == -1, "filesize of unopened fd 5");
  CHECK(filesize(128) == -1, "filesize of invalid fd 128");

  quiet = false;
}
