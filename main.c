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
#include "hashmap.c"
#include "hashtable.c"

#define BLOCK_SIZE 4096

static int (*libc_write)(int fd, const void *buf, size_t count);
static int (*libc_pwrite)(int fd, const void *buf, size_t count, off_t offset);
static int (*libc_read)(int fd, void *buf, size_t count);
static int (*libc_pread)(int fd, void *buf, size_t count, off_t offset);

#define RESOLVE_SYMBOL(name)                                                   \
    libc_##name = dlsym(RTLD_NEXT, #name);                                     \
    if (!libc_##name || dlerror()) {                                           \
        fprintf(stderr, "undeclared symbol `" #name "`\n");                    \
        exit(EXIT_FAILURE);                                                    \
    };

enum Operation {
    WRITE,
    PWRITE,
    READ,
    PREAD,
};

void __attribute__((constructor)) libwritededuper_init(void) {
    if ((hash_table = malloc(sizeof(HashEntry *) * pow(2, 32))) == NULL) {
        fprintf(stderr, "couldn't allocate memory: errno %d\n", errno);
        exit(EXIT_FAILURE);
    }

    working_fds = hashmap_new(sizeof(WorkingFd), 0, 0, 0, working_fd_hash,
                              working_fd_compare, NULL, NULL);

    RESOLVE_SYMBOL(write);
    RESOLVE_SYMBOL(pwrite);
    RESOLVE_SYMBOL(read);
    RESOLVE_SYMBOL(pread);
}

ssize_t handle_fallback_write(enum Operation type, int fd, const void *buf,
                              size_t count, off_t offset) {
    if (type == WRITE) {
        return (*libc_write)(fd, buf, count);
    } else if (type == PWRITE) {
        return (*libc_pwrite)(fd, buf, count, offset);
    }
    return -1;
}

ssize_t handle_write(enum Operation type, int fd, const void *buf, size_t count,
                     off_t offset) {
    if ((fcntl(fd, F_GETFL) & O_APPEND) == O_APPEND)
        return handle_fallback_write(type, fd, buf, count, offset);

    if (type == WRITE)
        offset = lseek(fd, 0, SEEK_CUR);

    if (count < BLOCK_SIZE || offset % BLOCK_SIZE != 0)
        return handle_fallback_write(type, fd, buf, count, offset);

    char path[4096] = {0};
    char fd_link[4096] = {0};
    sprintf(fd_link, "/proc/self/fd/%d", fd);
    if (!readlink(fd_link, path, 4095)) {
        fprintf(stderr, "couldn't readlink on file descriptor %d: errno %d\n",
                fd, errno);
        return handle_fallback_write(type, fd, buf, count, offset);
    };

    HashEntry *hash_entry;
    ssize_t written, total_written = 0;
    unsigned char block_buf[BLOCK_SIZE];

    for (ssize_t block_offset = 0; (block_offset + BLOCK_SIZE) <= count;
         block_offset += BLOCK_SIZE) {
        memcpy(block_buf, &buf[block_offset], BLOCK_SIZE);
        uint32_t hash = calculate_crc32c(0, block_buf, BLOCK_SIZE);

        if ((hash_entry = hash_table[hash]) == NULL) {
        fallback_write:
            hash_entry = malloc(sizeof(HashEntry));
            strcpy(hash_entry->path, path);
            hash_entry->offset = offset / BLOCK_SIZE;
            hash_table[hash] = hash_entry;

            if ((written = handle_fallback_write(type, fd, block_buf,
                                                 BLOCK_SIZE, offset)) < 0) {
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
                goto fallback_write;

            off_t in_offset = hash_entry->offset * BLOCK_SIZE;

            unsigned char in_buf[BLOCK_SIZE];
            if ((libc_pread)(in_fd, in_buf, BLOCK_SIZE, in_offset) < BLOCK_SIZE)
                goto fallback_write;
            if (memcmp(block_buf, in_buf, BLOCK_SIZE) != 0)
                goto fallback_write;

            if ((written = copy_file_range(in_fd, &in_offset, fd, &offset,
                                           BLOCK_SIZE, 0)) < 0) {
                fprintf(stderr,
                        "couldn't copy_file_range on file descriptor %d: errno "
                        "%d\n",
                        fd, errno);
                return -1;
            }
            if (lseek(fd, written, SEEK_CUR) == -1) {
                fprintf(stderr,
                        "couldn't lseek %ld bytes on file descriptor %d: errno "
                        "%d\n",
                        written, fd, errno);
                return -1;
            };
        }
        total_written += written;
    };

    return total_written;
}

ssize_t handle_fallback_read(enum Operation type, int fd, void *buf,
                             size_t count, off_t offset) {
    if (type == READ) {
        return (*libc_read)(fd, buf, count);
    } else if (type == PREAD) {
        return (*libc_pread)(fd, buf, count, offset);
    }
    return -1;
}

ssize_t handle_read(enum Operation type, int fd, void *buf, size_t count,
                    off_t offset) {
    if (type == READ)
        offset = lseek(fd, 0, SEEK_CUR);

    if (count < BLOCK_SIZE || offset % BLOCK_SIZE != 0)
        return handle_fallback_read(type, fd, buf, count, offset);

    char path[4096] = {0};
    char fd_link[4096] = {0};
    sprintf(fd_link, "/proc/self/fd/%d", fd);
    if (!readlink(fd_link, path, 4095)) {
        fprintf(stderr, "couldn't readlink on file descriptor %d: errno %d\n",
                fd, errno);
        return handle_fallback_read(type, fd, buf, count, offset);
    };

    count = handle_fallback_read(type, fd, buf, count, offset);

    HashEntry *hash_entry;
    unsigned char block_buf[BLOCK_SIZE];

    for (ssize_t block_offset = 0; (block_offset + BLOCK_SIZE) <= count;
         block_offset += BLOCK_SIZE) {
        memcpy(block_buf, &buf[block_offset], BLOCK_SIZE);
        uint32_t hash = calculate_crc32c(0, block_buf, BLOCK_SIZE);

        hash_entry = malloc(sizeof(HashEntry));
        strcpy(hash_entry->path, path);
        hash_entry->offset = offset / BLOCK_SIZE;
        hash_table[hash] = hash_entry;

        offset += BLOCK_SIZE;
    };

    return count;
}

ssize_t write(int fd, const void *buf, size_t count) {
    return handle_write(WRITE, fd, buf, count, -1);
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
    return handle_write(PWRITE, fd, buf, count, offset);
}

ssize_t read(int fd, void *buf, size_t count) {
    return handle_read(READ, fd, buf, count, -1);
}

ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
    return handle_read(PREAD, fd, buf, count, offset);
}
