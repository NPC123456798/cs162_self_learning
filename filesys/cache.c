#include "filesys/cache.h"
#include "threads/malloc.h"
#include "filesys/filesys.h"
#include <debug.h>

#define CACHE_SIZE 64

static struct cache_block cache[CACHE_SIZE];
static struct list cache_list; // store all stored cache block in list
static struct lock cache_lock;
static struct list_elem* clock_hand;

void cache_init(void) {
    lock_init(&cache_lock);
    list_init(&cache_list);

    for (int i = 0; i < CACHE_SIZE; i++) {
        struct cache_block* b = &cache[i];
        rw_lock_init(&b->rw_lock);
        cond_init(&b->waiters);
        b->sector = 0;
        b->valid = false;
        b->dirty = false;
        b->accessed = false;
        b->ref_cnt = 0;
        b->loading = false;
        list_push_back(&cache_list, &b->elem); // push all cache block into list
    }

    clock_hand = list_begin(&cache_list);
}


/* seek block in cache by using sector number, return block ptr or NULL
   caller must has  cache_lock */
static struct cache_block* cache_lookup_locked(block_sector_t sector) {
    struct list_elem* e;
    for (e = list_begin(&cache_list); e != list_end(&cache_list); e = list_next(e)) {
        struct cache_block* b = list_entry(e, struct cache_block, elem);
        if (b->valid && b->sector == sector) {
            return b;
        }
    }
    return NULL;
}

static struct cache_block* cache_evict_locked(void) {
    ASSERT(lock_held_by_current_thread(&cache_lock));

    for (int i = 0; i < CACHE_SIZE; i++) {
        struct cache_block* b = list_entry(clock_hand, struct cache_block, elem);
        clock_hand = list_next(clock_hand);
        if (clock_hand == list_end(&cache_list))
            clock_hand = list_begin(&cache_list);

        if (b->ref_cnt == 0) {
            if (b->accessed) {
                b->accessed = false;   // give second chance
            } else {
                return b;
            }
        }
    }
    PANIC("No evictable cache block");
}
struct cache_block* cache_get_block(block_sector_t sector, bool exclusive) {
    // this is necessary for avoiding situation of searching sector but the block's sector state is changed by the next eviction logic 
    // for search evicted block and cache hit block so  searching and block state change operation must in the same global cache lock protection.
    lock_acquire(&cache_lock);

    // 1. try to hit, searching
    struct cache_block* b = cache_lookup_locked(sector);
    if (b != NULL) {
        b->ref_cnt++;
        // get block lock(read write lock) 
        rw_lock_acquire(&b->rw_lock, exclusive ? RW_WRITER : RW_READER);
        lock_release(&cache_lock);

        // if block is loading waiting until loading finish 
        lock_acquire(&b->state_lock);
        while (b->loading) {
            cond_wait(&b->waiters, &b->state_lock);
        }
        lock_release(&b->state_lock);
        return b;
    }

    // 2. not hit and select a evicted block, searching
    b = cache_evict_locked();
    // keep old sector number used in write back 
    block_sector_t old_sector = b->sector;
    bool old_dirty = b->dirty;

    // update block information in the cache_lock's protection  , state change
    b->sector = sector;
    b->valid = false;
    b->loading = true;
    b->ref_cnt = 1;
    b->dirty = false;           // new data not be written so its not dirty at start
    b->accessed = true;         // loaded block is thought as accessed at recent 


    // release cache lock because write back and read is protected by loading flag and ref_cnt.
    // also because searching and state change operation is finished in one thread
    lock_release(&cache_lock);

    // 3. if evicted block is dirty write back data at first 
    if (old_dirty) {
        block_write(fs_device, old_sector, b->data);
    }

    // 4. read new sector data from disk  

    block_read(fs_device, sector, b->data);

    // 5. update state: load finished
    lock_acquire(&b->state_lock);
    b->valid = true;
    b->loading = false;
    lock_release(&b->state_lock);
    cond_broadcast(&b->waiters, &b->state_lock);   // awake up all threads waiting this block 

    // 6. get rwlock by exclusive and then return 
    rw_lock_acquire(&b->rw_lock, exclusive ? RW_WRITER : RW_READER);
    return b;
}


void cache_release_block(struct cache_block* b, bool exclusive) {
    // release rwlock
    rw_lock_release(&b->rw_lock, exclusive ? RW_WRITER : RW_READER);

    // subtract ref_cnt 
    lock_acquire(&b->state_lock);
    ASSERT(b->ref_cnt > 0);
    b->ref_cnt--;
    lock_release(&b->state_lock);
}