#include "filesys/free-map.h"
#include <bitmap.h>
#include <debug.h>
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/synch.h"

static struct file* free_map_file; /* Free map file. */
static struct bitmap* free_map;    /* Free map, one bit per sector. */
static struct lock free_map_lock;
static bool free_map_initializing = false;

/* Initializes the free map. */
void free_map_init(void) {
  free_map = bitmap_create(block_size(fs_device));
  if (free_map == NULL)
    PANIC("bitmap creation failed--file system device is too large");
  bitmap_mark(free_map, FREE_MAP_SECTOR);
  bitmap_mark(free_map, ROOT_DIR_SECTOR);
  lock_init(&free_map_lock);
}

/* Allocates CNT consecutive sectors from the free map and stores
   the first into *SECTORP.
   Returns true if successful, false if not enough consecutive
   sectors were available or if the free_map file could not be
   written. */
bool free_map_allocate(size_t cnt, block_sector_t* sectorp) {
  /* bitmap set should be exclusive because its shared resource or you will get situation that
  multiple files share same sector and modify one file make multiple file's content change */
  lock_acquire(&free_map_lock);
  block_sector_t sector = bitmap_scan_and_flip(free_map, 0, cnt, false);
  lock_release(&free_map_lock);

  /* here write back into disk doesn't need global lock protect because cache buffer
  has its fine-grained lock to protect same sector one time only one thread write it and 
  different sector multiple threads write at same time doesn't be a problem */
  if (sector != BITMAP_ERROR && free_map_file != NULL &&  !free_map_initializing && !bitmap_write(free_map, free_map_file)) {
    bitmap_set_multiple(free_map, sector, cnt, false);
    sector = BITMAP_ERROR;
  }
  if (sector != BITMAP_ERROR)
    *sectorp = sector;
  return sector != BITMAP_ERROR;
}

/* Makes CNT sectors starting at SECTOR available for use. */
void free_map_release(block_sector_t sector, size_t cnt) {
  ASSERT(bitmap_all(free_map, sector, cnt));
  // same reason with upon
  lock_acquire(&free_map_lock);
  bitmap_set_multiple(free_map, sector, cnt, false);
  lock_release(&free_map_lock);

  bitmap_write(free_map, free_map_file);
}

/* Opens the free map file and reads it from disk. */
void free_map_open(void) {
  free_map_file = file_open(inode_open(FREE_MAP_SECTOR));
  if (free_map_file == NULL)
    PANIC("can't open free map");
  if (!bitmap_read(free_map, free_map_file))
    PANIC("can't read free map");
}

/* Writes the free map to disk and closes the free map file. */
void free_map_close(void) { file_close(free_map_file); }

/* Creates a new free map file on disk and writes the free map to
   it. */
void free_map_create(void) {
      free_map_initializing = true;   // only set true on initialization duration

  /* Create inode. */
  if (!inode_create(FREE_MAP_SECTOR, bitmap_file_size(free_map), false))
    PANIC("free map creation failed");

  /* Write bitmap to file. */
  free_map_file = file_open(inode_open(FREE_MAP_SECTOR));
  if (free_map_file == NULL)
    PANIC("can't open free map");
  if (!bitmap_write(free_map, free_map_file))
    PANIC("can't write free map");
        free_map_initializing = false;  // create end, close it immediately 
}
