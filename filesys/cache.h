#ifndef FILESYS_FREE_MAP_H
#define FILESYS_FREE_MAP_H

#include "devices/block.h"
#include "threads/synch.h"

struct cache_block {
    block_sector_t sector;       /* this buffer block's corresponding disk sector number  */
    uint8_t data[BLOCK_SECTOR_SIZE]; /* sector data copy (512 bytes)  */
    bool valid;                  /* buffer block if include valid data  */
    bool dirty;                  /* data if changed and needed to be written to disk  */
    bool accessed;               /* accessed bit used in clock algorithm  */
    int ref_cnt;                 /* using this block's threads number to avoid eviction */
    bool loading;                /* if loading data from disk  */
    struct rw_lock rw_lock;            /* lock to protect this data block  */
    struct lock state_lock;     /*  protect valid, dirty, loading, ref_cnt */
    struct condition waiters;    /* thread queue of waiting the block state changed  */
    struct list_elem elem;       /* used in link the cache block into global cache list  */
};

void cache_init(void);


#endif /* filesys/cache.h */
