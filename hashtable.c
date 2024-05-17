typedef struct {
    char path[4096];
    int offset;
} HashEntry;

HashEntry **hash_table;
