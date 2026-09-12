#include <syscall.h>
#include "tests/lib.h"
#include "tests/main.h"

#define FILE_SIZE (16 * 1024)   /* 16 KiB = 32 blocks, fits entirely in the 64-block cache */
#define BLOCK_SIZE 512

static char buf[BLOCK_SIZE];

void test_main(void) {
    int hits, misses;

    CHECK(create("cache_test", FILE_SIZE), "create file");
    int fd = open("cache_test");
    CHECK(fd > 1, "open for write");
    for (int i = 0; i < FILE_SIZE / BLOCK_SIZE; i++) {
        for (int j = 0; j < BLOCK_SIZE; j++)
            buf[j] = (char)(i + j);
        if (write(fd, buf, BLOCK_SIZE) != BLOCK_SIZE)
            fail("write block failed");
    }
    close(fd);

    cache_reset();

    /* Cold cache read */
    fd = open("cache_test");
    CHECK(fd > 1, "open for cold read");
    for (int i = 0; i < FILE_SIZE / BLOCK_SIZE; i++) {
        if (read(fd, buf, BLOCK_SIZE) != BLOCK_SIZE)
            fail("cold read failed");
    }
    close(fd);
    cache_stats(&hits, &misses);
    int cold_hits = hits;
    int cold_misses = misses;

    /* Hot cache read (cache is not reset) */
    fd = open("cache_test");
    CHECK(fd > 1, "open for hot read");
    for (int i = 0; i < FILE_SIZE / BLOCK_SIZE; i++) {
        if (read(fd, buf, BLOCK_SIZE) != BLOCK_SIZE)
            fail("hot read failed");
    }
    close(fd);
    cache_stats(&hits, &misses);
    int hot_hits = hits - cold_hits;
    int hot_misses = misses - cold_misses;

    /* Verify: the hot cache should have significantly fewer misses
       and more hits than the cold cache. The exact numbers fluctuate
       due to metadata access and background flush timing, so we only
       check the relative improvement. */
    if (hot_misses >= cold_misses)
        fail("hot cache did not reduce miss count");
    if (hot_hits <= cold_hits)
        fail("hot cache did not improve hit count");
    if (hot_misses != 0)
        fail("hot cache should have zero misses");

    msg("cache hit rate verified");
    msg("cache hit rate test passed");
}