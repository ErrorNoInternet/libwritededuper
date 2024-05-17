#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
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
#include "hashmap.c"

#define BLOCK_SIZE 4096

static int (*libc_open)(const char *, int, ...);
static int (*libc_write)(int fd, const void *buf, size_t count);

void __attribute__((constructor)) writedduper_init(void) {
    if ((hash_table = malloc(sizeof(HashEntry *) * pow(2, 32))) == NULL) {
        fprintf(stderr, "couldn't allocate memory: errno %d\n", errno);
        exit(EXIT_FAILURE);
    }

    working_fds = hashmap_new(sizeof(WorkingFd), 0, 0, 0, working_fd_hash,
                              working_fd_compare, NULL, NULL);

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
    if ((fcntl(fd, F_GETFL) & O_APPEND) == O_APPEND)
        return (*libc_write)(fd, buf, len);

    off_t position = lseek(fd, 0, SEEK_CUR);
    if (len < BLOCK_SIZE || len % BLOCK_SIZE != 0 || position % BLOCK_SIZE != 0)
        return (*libc_write)(fd, buf, len);

    char path[4096] = {0};
    char fd_link[4096];
    sprintf(fd_link, "/proc/self/fd/%d", fd);
    if (!readlink(fd_link, path, 4095)) {
        fprintf(stderr, "couldn't readlink on file descriptor %d: errno %d\n",
                fd, errno);
        return (*libc_write)(fd, buf, len);
    };

    HashEntry *hash_entry;
    ssize_t written, total_written = 0;
    unsigned char sbuf[BLOCK_SIZE];

    for (int offset = 0; offset < len; offset += BLOCK_SIZE) {
        memcpy(sbuf, &buf[offset], BLOCK_SIZE);
        uint32_t hash = calculate_crc32c(0, sbuf, BLOCK_SIZE);

        if ((hash_entry = hash_table[hash]) == NULL) {
        new_block:
            hash_entry = malloc(sizeof(HashEntry));
            strcpy(hash_entry->path, path);
            hash_entry->offset = position / BLOCK_SIZE;
            hash_table[hash] = hash_entry;

            if ((written = (*libc_write)(fd, sbuf, BLOCK_SIZE)) < 0) {
                fprintf(stderr,
                        "couldn't write to file descriptor %d: errno "
                        "%d\n",
                        fd, errno);
                return -1;
            };
            position += BLOCK_SIZE;
        } else {
            int in_fd = get_working_fd(hash_entry->path);
            if (in_fd < 0)
                goto new_block;
            off_t in_position = hash_entry->offset * BLOCK_SIZE;

            if ((written = copy_file_range(in_fd, &in_position, fd, &position,
                                           BLOCK_SIZE, 0)) < 0) {
                fprintf(stderr,
                        "couldn't copy_file_range on file descriptor %d: errno "
                        "%d\n",
                        fd, errno);
                return -1;
            }
        }
        total_written += written;
    };

    return total_written;
}
