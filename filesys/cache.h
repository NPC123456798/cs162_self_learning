#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

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
    bool writing_back;          /* mark is writing back  */
    struct rw_lock rw_lock;            /* lock to protect this data block  */
    struct condition waiters;    /* thread queue of waiting the block state changed  */
    struct list_elem elem;       /* used in link the cache block into global cache list  */
};

void cache_init(void);



/**
 * get cache block and add lock as the request's mode 
 *
 * @param sector   accessed disk sector index 
 * @param exclusive  if true, get writer lock to modify data 
 *                  if false, get shared reader lock to read data   
 * @return return cache block pointer and caller has the corresponding rw_lock 
 */
struct cache_block* cache_get_block(block_sector_t sector, bool exclusive);
void cache_release_block(struct cache_block* b, bool exclusive);
void cache_mark_dirty(struct cache_block* b); 
void cache_flush(void);

#endif /* filesys/cache.h */
