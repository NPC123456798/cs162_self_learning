#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/cache.h"
#include "filesys/path.h"
#include "threads/thread.h"

/* Partition that contains the file system. */
struct block* fs_device;

static void do_format(void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void filesys_init(bool format) {
  fs_device = block_get_role(BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC("No file system device found, can't initialize file system.");

  inode_init();
  free_map_init();
  cache_init();

  
  if (format)
    do_format();

  free_map_open();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void filesys_done(void) { 
  free_map_close(); 
  cache_flush();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool filesys_create(const char* name, off_t initial_size) {
  block_sector_t inode_sector = 0;
  struct dir* dir = dir_open_root();
  bool success = (dir != NULL && free_map_allocate(1, &inode_sector) &&
                  inode_create(inode_sector, initial_size, false) && dir_add(dir, name, inode_sector));
  if (!success && inode_sector != 0)
    free_map_release(inode_sector, 1);
  dir_close(dir);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file* filesys_open(const char* name) {
  struct dir* dir = dir_open_root();
  struct inode* inode = NULL;

  if (dir != NULL)
    dir_lookup(dir, name, &inode);
  dir_close(dir);

  return file_open(inode);
}

/* check directory if empty (only include . and .. entry )  */
static bool dir_is_empty(struct inode* dir_inode) {
    struct dir* dir = dir_open(inode_reopen(dir_inode));
    if (dir == NULL)
        return false;

    char name[NAME_MAX + 1];
    bool empty = true;
    while (dir_readdir(dir, name)) {
        if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
            empty = false;
            break;
        }
    }
    dir_close(dir);
    return empty;
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool filesys_remove(const char* path_name) {
  char last_name[NAME_MAX + 1];
  struct dir * cwd = thread_current()->pcb->cwd;
  struct dir* parent_dir = path_to_parent_dir(path_name,cwd, last_name);
  if (parent_dir == NULL)
      return false;

  /* refuse delete . or .. */
  if (strcmp(last_name, ".") == 0 || strcmp(last_name, "..") == 0) {
      dir_close(parent_dir);
      return false;
  }

  /* search target in parent directory  */
  block_sector_t target_sector;
  if (!dir_lookup(parent_dir, last_name, &target_sector)) {
      dir_close(parent_dir);
      return false;
  }

  /* refuse delete root directory (root directory's inode sector number is ROOT_DIR_SECTOR )     */
  if (target_sector == ROOT_DIR_SECTOR) {
      dir_close(parent_dir);
      return false;
  }

  /* open target inode and check its type */
  struct inode* target_inode = inode_open(target_sector);
  if (target_inode == NULL) {
      dir_close(parent_dir);
      return false;
  }

  bool success = false;
  if (inode_is_dir(target_inode)) {
      /* directory: must empty*/
      if (dir_is_empty(target_inode))
          success = dir_remove(parent_dir, last_name);
  } else {
      /* normal file: directly delete  */
      success = dir_remove(parent_dir, last_name);
  }

  inode_close(target_inode);
  dir_close(parent_dir);
  return success;
}

/* Formats the file system. */
static void do_format(void) {
  printf("Formatting file system...");
  free_map_create();
  if (!dir_create(ROOT_DIR_SECTOR, 16))
    PANIC("root directory creation failed");



  /* add  "." and ".." for root directory, they all point to itself  */
  struct dir *root_dir = dir_open(inode_open(ROOT_DIR_SECTOR));
  if (root_dir == NULL)
      PANIC("failed to open root directory for initialization");
  if (!dir_add(root_dir, ".", ROOT_DIR_SECTOR) ||
      !dir_add(root_dir, "..", ROOT_DIR_SECTOR))
      PANIC("failed to add . or .. to root directory");
  dir_close(root_dir);

  free_map_close();
  printf("done.\n");
}
