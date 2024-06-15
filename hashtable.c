struct HashEntry {
    char path[4096];
    int offset;
};

struct HashEntry **hash_table;
