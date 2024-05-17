#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "crc32.c"
#include "fd.c"
#include "hash.c"

#define BLOCK_SIZE 4096

static int (*libc_open)(const char *, int, ...);
static int (*libc_write)(int fd, const void *buf, size_t count);

void __attribute__((constructor)) writedupe_init(void) {
    if ((hash_table = malloc(sizeof(HashEntry *) * pow(2, 32))) == NULL) {
        fprintf(stderr, "couldn't allocate memory: errno %d\n", errno);
        exit(EXIT_FAILURE);
    }

    fd_path_cache = malloc(INT_MAX);

    libc_open = dlsym(RTLD_NEXT, "open");
    if (!libc_open || dlerror()) {
        fprintf(stderr, "undeclared symbol `open`\n");
        exit(EXIT_FAILURE);
    };

    libc_write = dlsym(RTLD_NEXT, "write");
    if (!libc_write || dlerror()) {
        fprintf(stderr, "undeclared symbol `write`\n");
        exit(EXIT_FAILURE);
    };
}

ssize_t write(int fd, const void *buf, size_t len) {
    struct stat statbuf;
    if (fstat(fd, &statbuf) == -1) {
        fprintf(stderr, "couldn't fstat file descriptor %d: errno %d\n", fd,
                errno);
        exit(EXIT_FAILURE);
    }
    off_t position = statbuf.st_size;

    if (len < BLOCK_SIZE || len % BLOCK_SIZE != 0 || position % BLOCK_SIZE != 0)
        return (*libc_write)(fd, buf, len);

    char *path = fd_path(fd);

    HashEntry *hash_entry;
    ssize_t just_written;
    ssize_t written = 0;
    unsigned char sbuf[BLOCK_SIZE] = {0};

    for (int offset = 0; offset < len; offset += BLOCK_SIZE) {
        memcpy(sbuf, &buf[offset], BLOCK_SIZE);
        uint32_t hash = calculate_crc32c(0, sbuf, BLOCK_SIZE);

        if ((hash_entry = hash_table[hash]) == NULL) {
            hash_entry = malloc(sizeof(HashEntry));
            strcpy(hash_entry->path, path);
            hash_entry->offset = position / BLOCK_SIZE;
            hash_table[hash] = hash_entry;

            if ((just_written = (*libc_write)(fd, sbuf, BLOCK_SIZE)) < 0) {
                fprintf(stderr,
                        "couldn't write to file descriptor %d: errno "
                        "%d\n",
                        fd, errno);
                exit(EXIT_FAILURE);
            };
            position += BLOCK_SIZE;
        } else {
            off_t in_position = hash_entry->offset * BLOCK_SIZE;
            if ((just_written = copy_file_range(fd, &in_position, fd, &position,
                                                BLOCK_SIZE, 0)) < 0) {
                fprintf(stderr,
                        "couldn't copy_file_range on file descriptor %d: errno "
                        "%d\n",
                        fd, errno);
                exit(EXIT_FAILURE);
            }
        }
        written += just_written;
    };

    return written;
}
