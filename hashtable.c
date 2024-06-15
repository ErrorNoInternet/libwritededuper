#include <stdio.h>

struct HashEntry {
    char path[4096];
    off_t offset;
};

struct HashEntry **hash_table;
