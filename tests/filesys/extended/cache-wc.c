#include <syscall.h>
#include "tests/lib.h"
#include "tests/main.h"

#define FILE_SIZE (64 * 1024)   /* 64 KiB = 128 blocks */

static char byte_buf[1];

void test_main(void) {
    /* 1. Record block device write count before the test */
    int start_writes = get_write_cnt();

    /* 2. Create a file and write 64 KiB byte by byte */
    CHECK(create("coalesce_test", 0), "create file");
    int fd = open("coalesce_test");
    CHECK(fd > 1, "open for write");

    for (int i = 0; i < FILE_SIZE; i++) {
        byte_buf[0] = (char)(i & 0xFF);
        if (write(fd, byte_buf, 1) != 1)
            fail("byte write failed");
    }
    close(fd);

    /* 3. Flush the cache so that all dirty blocks are written back to disk */
    cache_reset();

    /* 4. Compute the number of block device writes */
    int end_writes = get_write_cnt();
    int writes = end_writes - start_writes;

    /* 5. Verify: the number of writes should be far less than the number
          of bytes (65536) and within a reasonable range. The exact number
          depends on cache replacement timing and the background flush
          thread, so it fluctuates around 160-170. Therefore we do not
          print the exact value, only a fixed message. */
    if (writes > 300)
        fail("write coalescing failed: too many device writes");
    if (writes < 100)
        fail("write coalescing failed: too few device writes");

    msg("write coalescing verified");
    msg("cache write coalesce test passed");
}