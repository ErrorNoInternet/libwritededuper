#include <stdio.h>

struct TableEntry {
    char path[4096];
    off_t offset;
};

struct TableEntry **hash_table;
