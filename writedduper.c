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

static int (*libc_write)(int fd, const void *buf, size_t count);
static int (*libc_pwrite)(int fd, const void *buf, size_t count, off_t offset);

void __attribute__((constructor)) writedduper_init(void) {
    if ((hash_table = malloc(sizeof(HashEntry *) * pow(2, 32))) == NULL) {
        fprintf(stderr, "couldn't allocate memory: errno %d\n", errno);
        exit(EXIT_FAILURE);
    }

    working_fds = hashmap_new(sizeof(WorkingFd), 0, 0, 0, working_fd_hash,
                              working_fd_compare, NULL, NULL);

    libc_write = dlsym(RTLD_NEXT, "write");
    if (!libc_write || dlerror()) {
        fprintf(stderr, "undeclared symbol `write`\n");
        exit(EXIT_FAILURE);
    };

    libc_pwrite = dlsym(RTLD_NEXT, "pwrite");
    if (!libc_pwrite || dlerror()) {
        fprintf(stderr, "undeclared symbol `pwrite`\n");
        exit(EXIT_FAILURE);
    };
}

ssize_t write(int fd, const void *buf, size_t count) {
    if ((fcntl(fd, F_GETFL) & O_APPEND) == O_APPEND)
        return (*libc_write)(fd, buf, count);

    off_t offset = lseek(fd, 0, SEEK_CUR);
    if (count < BLOCK_SIZE || count % BLOCK_SIZE != 0 || offset % BLOCK_SIZE != 0)
        return (*libc_write)(fd, buf, count);

    char path[4096] = {0};
    char fd_link[4096];
    sprintf(fd_link, "/proc/self/fd/%d", fd);
    if (!readlink(fd_link, path, 4095)) {
        fprintf(stderr, "couldn't readlink on file descriptor %d: errno %d\n",
                fd, errno);
        return (*libc_write)(fd, buf, count);
    };

    HashEntry *hash_entry;
    ssize_t written, total_written = 0;
    unsigned char s_buf[BLOCK_SIZE];

    for (int s_off = 0; s_off < count; s_off += BLOCK_SIZE) {
        memcpy(s_buf, &buf[s_off], BLOCK_SIZE);
        uint32_t hash = calculate_crc32c(0, s_buf, BLOCK_SIZE);

        if ((hash_entry = hash_table[hash]) == NULL) {
        new_block:
            hash_entry = malloc(sizeof(HashEntry));
            strcpy(hash_entry->path, path);
            hash_entry->offset = offset / BLOCK_SIZE;
            hash_table[hash] = hash_entry;

            if ((written = (*libc_write)(fd, s_buf, BLOCK_SIZE)) < 0) {
                fprintf(stderr,
                        "couldn't write to file descriptor %d: errno "
                        "%d\n",
                        fd, errno);
                return -1;
            };
            offset += BLOCK_SIZE;
        } else {
            int in_fd = get_working_fd(hash_entry->path);
            if (in_fd < 0)
                goto new_block;
            off_t in_position = hash_entry->offset * BLOCK_SIZE;

            if ((written = copy_file_range(in_fd, &in_position, fd, &offset,
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

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
    if ((fcntl(fd, F_GETFL) & O_APPEND) == O_APPEND)
        return (*libc_pwrite)(fd, buf, count, offset);

    if (count < BLOCK_SIZE || count % BLOCK_SIZE != 0 || offset % BLOCK_SIZE != 0)
        return (*libc_pwrite)(fd, buf, count, offset);

    char path[4096] = {0};
    char fd_link[4096];
    sprintf(fd_link, "/proc/self/fd/%d", fd);
    if (!readlink(fd_link, path, 4095)) {
        fprintf(stderr, "couldn't readlink on file descriptor %d: errno %d\n",
                fd, errno);
        return (*libc_pwrite)(fd, buf, count, offset);
    };

    HashEntry *hash_entry;
    ssize_t written, total_written = 0;
    unsigned char s_buf[BLOCK_SIZE];

    for (int s_off = 0; s_off < count; s_off += BLOCK_SIZE) {
        memcpy(s_buf, &buf[s_off], BLOCK_SIZE);
        uint32_t hash = calculate_crc32c(0, s_buf, BLOCK_SIZE);

        if ((hash_entry = hash_table[hash]) == NULL) {
        new_block:
            hash_entry = malloc(sizeof(HashEntry));
            strcpy(hash_entry->path, path);
            hash_entry->offset = offset / BLOCK_SIZE;
            hash_table[hash] = hash_entry;

            if ((written = (*libc_pwrite)(fd, s_buf, BLOCK_SIZE, offset)) < 0) {
                fprintf(stderr,
                        "couldn't write to file descriptor %d: errno "
                        "%d\n",
                        fd, errno);
                return -1;
            };
            offset += BLOCK_SIZE;
        } else {
            int in_fd = get_working_fd(hash_entry->path);
            if (in_fd < 0)
                goto new_block;
            off_t in_position = hash_entry->offset * BLOCK_SIZE;

            if ((written = copy_file_range(in_fd, &in_position, fd, &offset,
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
