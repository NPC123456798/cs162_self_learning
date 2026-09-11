#ifndef FILESYS_PATH_H
#define FILESYS_PATH_H


#include "filesys/directory.h"


int get_next_part(char part[NAME_MAX + 1], const char** srcp);

#endif /* filesys/path.h */
