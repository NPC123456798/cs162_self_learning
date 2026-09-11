#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "filesys/cache.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44
#define DIRECT_BLOCK_COUNT 12
#define INDIRECT_BLOCK_ENTRIES (BLOCK_SECTOR_SIZE / sizeof(block_sector_t))  /* 128 */
#define INVALID_SECTOR ((block_sector_t) -1)



struct inode_block_pointers
{
    block_sector_t direct[DIRECT_BLOCK_COUNT];   /* direct block pointer */
    block_sector_t indirect;                     /* indirect block pointer  */
    block_sector_t double_indirect;              /* double indirect block pointer */
};


/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk {
    struct inode_block_pointers inode_pointers;
    off_t length;                                /* file size(bytes)  */
    bool is_dir;
    unsigned magic;                              /* magic number */
    /* fill into 512 bytes */
    uint32_t unused[(BLOCK_SECTOR_SIZE
                    - sizeof(struct inode_block_pointers)
                    - sizeof(bool)
                    - sizeof(off_t)
                    - sizeof(unsigned))
                    / sizeof(uint32_t)];
};

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t bytes_to_sectors(off_t size) { return DIV_ROUND_UP(size, BLOCK_SECTOR_SIZE); }

/* In-memory inode. */
struct inode {
  struct list_elem elem;  /* Element in inode list. */
  block_sector_t sector;  /* Sector number of disk location. */
  int open_cnt;           /* Number of openers. */
  bool removed;           /* True if deleted, false otherwise. */
  int deny_write_cnt;     /* 0: writes ok, >0: deny writes. */
  bool is_dir;
  off_t length;
  struct inode_block_pointers block_ptrs;    /* cached  block pointers  */
  struct rw_lock inode_lock;                     /* protect length and block pointer */
  bool loading;
  struct condition waiters;
};

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t byte_to_sector(const struct inode* inode, off_t pos) {
  ASSERT(inode != NULL);
  if (pos < inode->length)
    return pos / BLOCK_SECTOR_SIZE;
  else
    return -1;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;
static struct lock inode_list_lock;

/* Initializes the inode module. */
void inode_init(void) { 
  list_init(&open_inodes);
  lock_init(&inode_list_lock);
}



static bool inode_allocate_block(struct inode_block_pointers *disk_inode, off_t block_idx) {
    block_sector_t new_sector;

    /* 1. direct block */
    if (block_idx < DIRECT_BLOCK_COUNT) {
        if (!free_map_allocate(1, &new_sector))
            return false;
        disk_inode->direct[block_idx] = new_sector;
        return true;
    }

    block_idx -= DIRECT_BLOCK_COUNT;

    /* 2. indirect block  */
    if (block_idx < INDIRECT_BLOCK_ENTRIES) {
        /* if indirect block doesn't allocated, allocate it and init it  */
        if (disk_inode->indirect == INVALID_SECTOR) {
            if (!free_map_allocate(1, &disk_inode->indirect))
                return false;

            struct cache_block *ib = cache_get_block(disk_inode->indirect, RW_WRITER);
            block_sector_t *entries = (block_sector_t *)ib->data;
            for (size_t i = 0; i < INDIRECT_BLOCK_ENTRIES; i++)
                entries[i] = INVALID_SECTOR;
            cache_mark_dirty(ib);
            cache_release_block(ib, RW_WRITER);
        }

        /* allocate data block */
        if (!free_map_allocate(1, &new_sector))
            return false;

        /* record entry in indirect block  */
        struct cache_block *ib = cache_get_block(disk_inode->indirect, RW_WRITER);
        block_sector_t *entries = (block_sector_t *)ib->data;
        entries[block_idx] = new_sector;
        cache_mark_dirty(ib);
        cache_release_block(ib, RW_WRITER);
        return true;
    }

    block_idx -= INDIRECT_BLOCK_ENTRIES;

    /* 3. double indirect  */
    size_t dbl_idx = block_idx / INDIRECT_BLOCK_ENTRIES;
    size_t ind_idx = block_idx % INDIRECT_BLOCK_ENTRIES;

    /* if double indirect block doesn't be allocate, allocate it and init it  */
    if (disk_inode->double_indirect == INVALID_SECTOR) {
        if (!free_map_allocate(1, &disk_inode->double_indirect))
            return false;

        struct cache_block *db = cache_get_block(disk_inode->double_indirect, RW_WRITER);
        block_sector_t *dbl_entries = (block_sector_t *)db->data;
        for (size_t i = 0; i < INDIRECT_BLOCK_ENTRIES; i++)
            dbl_entries[i] = INVALID_SECTOR;
        cache_mark_dirty(db);
        cache_release_block(db, RW_WRITER);
    }

    /* read double indirect  block and find the corresponding indirect block  */
    struct cache_block *db = cache_get_block(disk_inode->double_indirect, RW_READER);
    block_sector_t *dbl_entries = (block_sector_t *)db->data;
    block_sector_t indirect_sector = dbl_entries[dbl_idx];
    cache_release_block(db, RW_READER);

    /* if indirect block doesn't allocated, allocate it and init it */
    if (indirect_sector == INVALID_SECTOR) {
        if (!free_map_allocate(1, &indirect_sector))
            return false;

        struct cache_block *ib = cache_get_block(indirect_sector, RW_WRITER);
        block_sector_t *ind_entries = (block_sector_t *)ib->data;
        for (size_t i = 0; i < INDIRECT_BLOCK_ENTRIES; i++)
            ind_entries[i] = INVALID_SECTOR;
        cache_mark_dirty(ib);
        cache_release_block(ib, RW_WRITER);

        /* update entry in double indirect block  */
        db = cache_get_block(disk_inode->double_indirect, RW_WRITER);
        dbl_entries = (block_sector_t *)db->data;
        dbl_entries[dbl_idx] = indirect_sector;
        cache_mark_dirty(db);
        cache_release_block(db, RW_WRITER);
    }

    /* allocate data block  */
    if (!free_map_allocate(1, &new_sector))
        return false;

    /* record in indirect block  */
    struct cache_block *ib = cache_get_block(indirect_sector, RW_WRITER);
    block_sector_t *ind_entries = (block_sector_t *)ib->data;
    ind_entries[ind_idx] = new_sector;
    cache_mark_dirty(ib);
    cache_release_block(ib, RW_WRITER);

    return true;
}


static block_sector_t inode_get_data_sector(const struct inode_block_pointers *disk_inode, off_t block_idx) {
    if (block_idx < 0)
    {
        return INVALID_SECTOR;
    }
    

    if (block_idx < DIRECT_BLOCK_COUNT)
        return disk_inode->direct[block_idx];

    block_idx -= DIRECT_BLOCK_COUNT;

    if (block_idx < INDIRECT_BLOCK_ENTRIES) {
        if (disk_inode->indirect == INVALID_SECTOR)
            return INVALID_SECTOR;
        struct cache_block *ib = cache_get_block(disk_inode->indirect, RW_READER);
        block_sector_t *entries = (block_sector_t *)ib->data;
        block_sector_t sec = entries[block_idx];
        cache_release_block(ib, RW_READER);
        return sec;
    }

    block_idx -= INDIRECT_BLOCK_ENTRIES;

    size_t dbl_idx = block_idx / INDIRECT_BLOCK_ENTRIES;
    size_t ind_idx = block_idx % INDIRECT_BLOCK_ENTRIES;

    if (disk_inode->double_indirect == INVALID_SECTOR)
        return INVALID_SECTOR;

    struct cache_block *db = cache_get_block(disk_inode->double_indirect, RW_READER);
    block_sector_t *dbl_entries = (block_sector_t *)db->data;
    block_sector_t ind_sector = dbl_entries[dbl_idx];
    cache_release_block(db, RW_READER);

    if (ind_sector == INVALID_SECTOR)
        return INVALID_SECTOR;

    struct cache_block *ib = cache_get_block(ind_sector, RW_READER);
    block_sector_t *ind_entries = (block_sector_t *)ib->data;
    block_sector_t sec = ind_entries[ind_idx];
    cache_release_block(ib, RW_READER);
    return sec;
}


static void inode_free_blocks(struct inode_block_pointers *disk_inode) {
    /* release direct block */
    for (int i = 0; i < DIRECT_BLOCK_COUNT; i++) {
        if (disk_inode->direct[i] != INVALID_SECTOR) {
            free_map_release(disk_inode->direct[i], 1);
            disk_inode->direct[i] = INVALID_SECTOR;
        }
    }

    /* release indirect block and its data block  */
    if (disk_inode->indirect != INVALID_SECTOR) {
        struct cache_block *ib = cache_get_block(disk_inode->indirect, RW_READER);
        block_sector_t *entries = (block_sector_t *)ib->data;
        for (size_t i = 0; i < INDIRECT_BLOCK_ENTRIES; i++) {
            if (entries[i] != INVALID_SECTOR) {
                free_map_release(entries[i], 1);
            }
        }
        cache_release_block(ib, RW_READER);
        free_map_release(disk_inode->indirect, 1);
        disk_inode->indirect = INVALID_SECTOR;
    }

    /* release double indirect block and its all subordinate blocks  */
    if (disk_inode->double_indirect != INVALID_SECTOR) {
        struct cache_block *db = cache_get_block(disk_inode->double_indirect, RW_READER);
        block_sector_t *dbl_entries = (block_sector_t *)db->data;
        for (size_t i = 0; i < INDIRECT_BLOCK_ENTRIES; i++) {
            block_sector_t ind_sector = dbl_entries[i];
            if (ind_sector != INVALID_SECTOR) {
                struct cache_block *ib = cache_get_block(ind_sector, RW_READER);
                block_sector_t *ind_entries = (block_sector_t *)ib->data;
                for (size_t j = 0; j < INDIRECT_BLOCK_ENTRIES; j++) {
                    if (ind_entries[j] != INVALID_SECTOR) {
                        free_map_release(ind_entries[j], 1);
                    }
                }
                cache_release_block(ib, RW_READER);
                free_map_release(ind_sector, 1);
            }
        }
        cache_release_block(db, RW_READER);
        free_map_release(disk_inode->double_indirect, 1);
        disk_inode->double_indirect = INVALID_SECTOR;
    }
}


static void inode_release_block_at(struct inode_block_pointers *bp, off_t block_idx) {
    block_sector_t sec = INVALID_SECTOR;

    if (block_idx < 0)
    {
        return;
    }
    

    if (block_idx < DIRECT_BLOCK_COUNT) {
        sec = bp->direct[block_idx];
        bp->direct[block_idx] = INVALID_SECTOR;
        if (sec != INVALID_SECTOR)
            free_map_release(sec, 1);
        return;
    }

    block_idx -= DIRECT_BLOCK_COUNT;

    if (block_idx < INDIRECT_BLOCK_ENTRIES) {
        if (bp->indirect == INVALID_SECTOR)
            return;
        struct cache_block *ib = cache_get_block(bp->indirect, RW_WRITER);
        block_sector_t *entries = (block_sector_t *)ib->data;
        sec = entries[block_idx];
        entries[block_idx] = INVALID_SECTOR;

        // check indirect block if all entries invalid 
        bool empty = true;
        for (int i = 0; i < INDIRECT_BLOCK_ENTRIES; i++) {
            if (entries[i] != INVALID_SECTOR) {
                empty = false;
                break;
            }
        }
        cache_mark_dirty(ib);
        cache_release_block(ib, RW_WRITER);

        if (sec != INVALID_SECTOR)
            free_map_release(sec, 1);

        if (empty && bp->indirect != INVALID_SECTOR) {
            free_map_release(bp->indirect, 1);
            bp->indirect = INVALID_SECTOR;
        }
        return;
    }

    block_idx -= INDIRECT_BLOCK_ENTRIES;

    size_t dbl_idx = block_idx / INDIRECT_BLOCK_ENTRIES;
    size_t ind_idx = block_idx % INDIRECT_BLOCK_ENTRIES;

    if (bp->double_indirect == INVALID_SECTOR)
        return;

    struct cache_block *db = cache_get_block(bp->double_indirect, RW_WRITER);
    block_sector_t *dbl_entries = (block_sector_t *)db->data;
    block_sector_t ind_sector = dbl_entries[dbl_idx];
    cache_release_block(db, RW_WRITER);

    if (ind_sector == INVALID_SECTOR) {
        return;
    }

    struct cache_block *ib = cache_get_block(ind_sector, RW_WRITER);
    block_sector_t *ind_entries = (block_sector_t *)ib->data;
    sec = ind_entries[ind_idx];
    ind_entries[ind_idx] = INVALID_SECTOR;

    // check indirect block if empty 
    bool empty = true;
    for (int i = 0; i < INDIRECT_BLOCK_ENTRIES; i++) {
        if (ind_entries[i] != INVALID_SECTOR) {
            empty = false;
            break;
        }
    }
    cache_mark_dirty(ib);
    cache_release_block(ib, RW_WRITER);

    if (sec != INVALID_SECTOR)
        free_map_release(sec, 1);

    if (empty) {
        free_map_release(ind_sector, 1);
        bool db_empty = true;
        // update double indirect block's entry
        db = cache_get_block(bp->double_indirect, RW_WRITER);
        dbl_entries = (block_sector_t *)db->data;
        dbl_entries[dbl_idx] = INVALID_SECTOR;
        // check if double indirect block empty 
        for (int i = 0; i < INDIRECT_BLOCK_ENTRIES; i++) {
            if (dbl_entries[i] != INVALID_SECTOR) {
                db_empty = false;
                break;
            }
        }
        cache_mark_dirty(db);
        cache_release_block(db, RW_WRITER);

        if (db_empty) {
            free_map_release(bp->double_indirect, 1);
            bp->double_indirect = INVALID_SECTOR;
        }
    }
}


/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool inode_create(block_sector_t sector, off_t length, bool is_dir) {
    struct inode_disk* disk_inode = NULL;

    ASSERT(length >= 0);

    /* If this assertion fails, the inode structure is not exactly
        one sector in size, and you should fix that. */
    ASSERT(sizeof *disk_inode == BLOCK_SECTOR_SIZE);

    disk_inode = calloc(1, sizeof *disk_inode);
    if (disk_inode == NULL)
            return false;

    /* initialize  inode meta data */
    disk_inode->length = length;
    disk_inode->is_dir = is_dir;
    disk_inode->magic = INODE_MAGIC;

    /* initialize all block pointer into invalid value */
    for (int i = 0; i < DIRECT_BLOCK_COUNT; i++)
        disk_inode->inode_pointers.direct[i] = INVALID_SECTOR;
    disk_inode->inode_pointers.indirect = INVALID_SECTOR;
    disk_inode->inode_pointers.double_indirect = INVALID_SECTOR;

    /* compute numbers of data blocks need to be allcoated */
    size_t num_blocks = bytes_to_sectors(length);

    /* allocate logic block one by one  */
    bool ok = true;
    for (size_t block_idx = 0; block_idx < num_blocks; block_idx++) {
        if (!inode_allocate_block(&disk_inode->inode_pointers, block_idx)) {
            ok = false;
            break;
        }
    }

    if (!ok) {
        /* allocate fail, free allocated blocks  */
        inode_free_blocks(&disk_inode->inode_pointers);
        free(disk_inode);
        return false;
    }

    /* write inode meta data into cache   */
    struct cache_block *b = cache_get_block(sector, RW_WRITER);
    memcpy(b->data, disk_inode, sizeof *disk_inode);
    cache_mark_dirty(b);
    cache_release_block(b, RW_WRITER);

    /* set all data blocks as zeros, it means now the file system is non-sparse */
    for (size_t block_idx = 0; block_idx < num_blocks; block_idx++) {
        block_sector_t data_sector = inode_get_data_sector(&disk_inode->inode_pointers, block_idx);
        ASSERT(data_sector != INVALID_SECTOR);

        struct cache_block *data_block = cache_get_block(data_sector, RW_WRITER);
        memset(data_block->data, 0, BLOCK_SECTOR_SIZE);
        cache_mark_dirty(data_block);
        cache_release_block(data_block, RW_WRITER);
    }

    free(disk_inode);
    return true;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode* inode_open(block_sector_t sector) {
  struct list_elem* e;
  struct inode* inode;

  lock_acquire(&inode_list_lock);

  /* Check whether this inode is already open. */
  for (e = list_begin(&open_inodes); e != list_end(&open_inodes); e = list_next(e)) {
    inode = list_entry(e, struct inode, elem);
    if (inode->sector == sector) {
       /* if is loading wait it finished  */
      while (inode->loading) {
          cond_wait(&inode->waiters, &inode_list_lock);
      }
      /* reopen operation */
      inode->open_cnt++;
      lock_release(&inode_list_lock);
      return inode;
    }
  }

  /* Allocate memory. */
  inode = malloc(sizeof *inode);
  if (inode == NULL) {
    lock_release(&inode_list_lock);
    return NULL;
  }

  /* Initialize. */
  rw_lock_init(&inode->inode_lock);
  cond_init(&inode->waiters);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  inode->loading = true;

  list_push_front(&open_inodes, &inode->elem);
  lock_release(&inode_list_lock);


  /* read  inode meta data by accessing cache */
  struct inode_disk *disk_inode;
  struct cache_block* b = cache_get_block(sector, RW_READER);
  disk_inode = (struct inode_disk *)&b->data;

  inode->length = disk_inode->length;
  inode->is_dir = disk_inode->is_dir;
  inode->block_ptrs = disk_inode->inode_pointers;
  cache_release_block(b, RW_READER);

  /* regain lock and mark load finished also waking up waiters  */
  lock_acquire(&inode_list_lock);
  inode->loading = false;
  cond_broadcast(&inode->waiters, &inode_list_lock);
  lock_release(&inode_list_lock);

  
  return inode;
}

/* Reopens and returns INODE. */
struct inode* inode_reopen(struct inode* inode) {
  if (inode != NULL) {
    lock_acquire(&inode_list_lock);

    inode->open_cnt++;

    lock_release(&inode_list_lock);
  }
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t inode_get_inumber(const struct inode* inode) { return inode->sector; }

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void inode_close(struct inode* inode) {
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  lock_acquire(&inode_list_lock);
  bool last = (--inode->open_cnt == 0);

  /* Remove from inode list and release lock. */
  if (last) {
    list_remove(&inode->elem);
  }

  bool should_free_blocks = (last && inode->removed);
  lock_release(&inode_list_lock);

  /* Release resources if this was the last opener. */
  if (last) {
    /* Deallocate blocks if removed. */
    if (should_free_blocks) {
        inode_free_blocks(&inode->block_ptrs);
        free_map_release(inode->sector, 1);
    }
    free(inode);
  }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void inode_remove(struct inode* inode) {
  ASSERT(inode != NULL);
  lock_acquire(&inode_list_lock);
  inode->removed = true;
  lock_release(&inode_list_lock);
}



/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t inode_read_at(struct inode* inode, void* buffer_, off_t size, off_t offset) {
  uint8_t* buffer = buffer_;
  off_t bytes_read = 0;

  rw_lock_acquire(&inode->inode_lock, RW_READER);
  while (size > 0) {
    /* Disk sector to read, starting byte offset within sector. */
    block_sector_t sector_idx = inode_get_data_sector(&inode->block_ptrs, byte_to_sector(inode, offset));
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left; // if file will come to end and the left 
    // of file less than sector_left just choose file left so that we don't jump over the file

    /* Number of bytes to actually copy out of this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    if (sector_idx == INVALID_SECTOR) {
        memset(buffer + bytes_read, 0, chunk_size);
    } else {
        /* get cache block in reader mode and copy bytes from cache data  */
        struct cache_block* b = cache_get_block(sector_idx, RW_READER);
        memcpy(buffer + bytes_read, b->data + sector_ofs, chunk_size);
        cache_release_block(b, RW_READER);
    }

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_read += chunk_size;
  }
  rw_lock_release(&inode->inode_lock, RW_READER);
  return bytes_read;
}



static bool inode_extend(struct inode *inode, off_t new_length) {
    // caller must has  inode->inode_lock
    if (new_length <= inode->length) return true;

    off_t old_blocks = bytes_to_sectors(inode->length);
    off_t new_blocks = bytes_to_sectors(new_length);

    off_t allocated = old_blocks;  // successfully allocated blocks' number  

    for (off_t i = old_blocks; i < new_blocks; i++) {
        if (!inode_allocate_block(&inode->block_ptrs, i)) {
            // free allocated blocks in this loop 
            for (off_t j = old_blocks; j < allocated; j++) {
                inode_release_block_at(&inode->block_ptrs, j);
            }
            return false;
        }
        allocated++;
    }

    inode->length = new_length;

    // write back inode into disk
    struct cache_block *b = cache_get_block(inode->sector, RW_WRITER);
    struct inode_disk *disk_inode = (struct inode_disk *)b->data;
    disk_inode->inode_pointers = inode->block_ptrs;
    disk_inode->length = inode->length;
    // keep magic 
    cache_mark_dirty(b);
    cache_release_block(b, RW_WRITER);

    return true;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t inode_write_at(struct inode* inode, const void* buffer_, off_t size, off_t offset) {
  const uint8_t* buffer = buffer_;
  off_t bytes_written = 0;

  if (inode->deny_write_cnt)
    return 0;
  rw_lock_acquire(&inode->inode_lock, RW_WRITER);

    // if write will beyond the file end, extend it first 
  if (offset + size > inode->length) {
    if (!inode_extend(inode, offset + size)) {
        rw_lock_release(&inode->inode_lock, RW_WRITER);
        return 0;   // extend fail return 0  
    }
  }

  while (size > 0) {
    /* Sector to write, starting byte offset within sector. */
    block_sector_t sector_idx = inode_get_data_sector(&inode->block_ptrs, byte_to_sector(inode, offset));
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually write into this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;


    if (sector_idx == INVALID_SECTOR)
    {
        break;
    }
    

    /* get write cache block (which will automatically load old
      data or keep cache content) modify it and mark dirty   */
    struct cache_block* b = cache_get_block(sector_idx, RW_WRITER);
    memcpy(b->data + sector_ofs, buffer + bytes_written, chunk_size);
    cache_mark_dirty(b);
    cache_release_block(b, RW_WRITER);


    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_written += chunk_size;
  }


  rw_lock_release(&inode->inode_lock, RW_WRITER);
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void inode_deny_write(struct inode* inode) {
  inode->deny_write_cnt++;
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void inode_allow_write(struct inode* inode) {
  ASSERT(inode->deny_write_cnt > 0);
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t inode_length(const struct inode* inode) { return inode->length; }

bool inode_is_dir(const struct inode *inode) {
    return inode->is_dir;
}