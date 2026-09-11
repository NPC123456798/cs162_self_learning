#ifndef FILESYS_PATH_H
#define FILESYS_PATH_H


#include "filesys/directory.h"

struct dir* path_to_parent_dir(const char* path, struct dir* start, char last_name[NAME_MAX + 1]);
int get_next_part(char part[NAME_MAX + 1], const char** srcp);

#endif /* filesys/path.h */
