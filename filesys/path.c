
#include "filesys/path.h"






/* Extracts a file name part from *SRCP into PART, and updates *SRCP so that the
   next call will return the next file name part. Returns 1 if successful, 0 at
   end of string, -1 for a too-long file name part. */
int get_next_part(char part[NAME_MAX + 1], const char** srcp) {
  const char* src = *srcp;
  char* dst = part;

  /* Skip leading slashes.  If it's all slashes, we're done. */
  while (*src == '/')
    src++;
  if (*src == '\0')
    return 0;

  /* Copy up to NAME_MAX character from SRC to DST.  Add null terminator. */
  while (*src != '/' && *src != '\0') {
    if (dst < part + NAME_MAX)
      *dst++ = *src;
    else
      return -1;
    src++;
  }
  *dst = '\0';

  /* Advance source pointer. */
  *srcp = src;
  return 1;
}

/* path_to_parent_dir
   analysis PATH, open parent directory and copy last segment name into  LAST_NAME¡£
   PATH can be absolute path or (start with '/') relative path 
   START is the start directory of relative directory (usually is cwd) 
   success time return opened target directory's parent directory and fail time will return NULL 
   caller be responsible for closing returned directory by using dir_close */
struct dir* path_to_parent_dir(const char* path, struct dir* start, char last_name[NAME_MAX + 1]) {
    if (path == NULL || start == NULL || last_name == NULL)
        return NULL;

    /*  const int the front means the p can be modified but the content of p pointing 
    to can't be modified by using *p because it means the content of p pointing to is constant, the const after char* means the pointer p 
    can't be modified but not means the content pointed by p can't be modified, same for all pointer type */
    const char* p = path;
    char part[NAME_MAX + 1];
    char prev_part[NAME_MAX + 1];

    /* make sure start directory, absolute path start with root and relative start with START  */
    struct dir* current;
    if (*p == '/')
        current = dir_open_root();
    else
        current = dir_reopen(start);
    if (current == NULL)
        return NULL;

    /* read first segment  */
    int result = get_next_part(part, &p);
    if (result != 1) {
        dir_close(current);
        return NULL;    /* empty path or only include  '/' */
    }
    strlcpy(prev_part, part, sizeof(prev_part));

    /* analysis segment by segment ,prev_part is has been read last segment, part is next segment  
       loop every time read new  part, and handle  prev_part as middleware
       when loop end ,prev_part is last segment   */
    while ((result = get_next_part(part, &p)) != 0) {
        if (result == -1) {
            /* someone component is too long  */
            dir_close(current);
            return NULL;
        }

        /* handle prev_part as middleware   */
        if (strcmp(prev_part, ".") == 0) {
            /* '.' keep cwd unchanged  */
        } else if (strcmp(prev_part, "..") == 0) {
            /* '..' switch to parent directory  */
            struct inode* parent_inode;
            if (!dir_lookup(current, "..", &parent_inode)) {
                dir_close(current);
                return NULL;
            }
            dir_close(current);
            current = dir_open(parent_inode);   /* consume inode reference */
            if (current == NULL)
                return NULL;
        } else {
            /* normal name: go into subdirectory  */
            struct inode* next_inode;
            if (!dir_lookup(current, prev_part, &next_inode)) {
                dir_close(current);
                return NULL;
            }
            if (!inode_is_dir(next_inode)) {
                inode_close(next_inode);
                dir_close(current);
                return NULL;
            }
            dir_close(current);
            current = dir_open(next_inode);     /* consume inode reference */
            if (current == NULL)
                return NULL;
        }

        /* prev_part move forward */
        strlcpy(prev_part, part, sizeof(prev_part));
    }

    /* prev_part is last segment name  */
    strlcpy(last_name, prev_part, NAME_MAX + 1);
    return current;
}

bool path_to_inode(const char* path, struct dir* start, struct inode** out) {
    char last_name[NAME_MAX + 1];
    struct dir* parent = path_to_parent_dir(path, start, last_name);
    if (parent == NULL)
        return false;

    struct inode* inode = NULL;
    bool ok = dir_lookup(parent, last_name, &inode);
    dir_close(parent);

    if (!ok)
        return false;
    *out = inode;
    return true;
}