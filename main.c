#define _GNU_SOURCE

#include <dlfcn.h>
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

static int libwritededuper_ready = 0;

static int (*libc_write)(int fd, const void *buf, size_t count);
static int (*libc_pwrite)(int fd, const void *buf, size_t count, off_t offset);
static int (*libc_read)(int fd, void *buf, size_t count);
static int (*libc_pread)(int fd, void *buf, size_t count, off_t offset);

#define RESOLVE_SYMBOL(name)                                                   \
    libc_##name = dlsym(RTLD_NEXT, #name);                                     \
    if (!libc_##name) {                                                        \
        fprintf(stderr,                                                        \
                "libwritededuper: undeclared symbol `" #name "`: %s\n",        \
                dlerror());                                                    \
        exit(EXIT_FAILURE);                                                    \
    };

void __attribute__((constructor)) libwritededuper_init(void) {
    if ((hash_table = malloc(sizeof(struct HashEntry *) * pow(2, 32))) ==
        NULL) {
        fprintf(stderr, "libwritededuper: couldn't allocate memory: %m\n");
        exit(EXIT_FAILURE);
    }

    working_fds = hashmap_new(sizeof(struct WorkingFd), 0, 0, 0,
                              working_fd_hash, working_fd_compare, NULL, NULL);

    RESOLVE_SYMBOL(write);
    RESOLVE_SYMBOL(pwrite);
    RESOLVE_SYMBOL(read);
    RESOLVE_SYMBOL(pread);

    libwritededuper_ready = 1;
}

enum Operation {
    WRITE,
    PWRITE,
    READ,
    PREAD,
};

ssize_t handle_fallback_write(enum Operation type, int fd, const void *buf,
                              size_t count, off_t offset) {
    if (type == WRITE) {
        return (*libc_write)(fd, buf, count);
    } else if (type == PWRITE) {
        return (*libc_pwrite)(fd, buf, count, offset);
    }
    return -1;
}

ssize_t handle_write(enum Operation type, int fd, const unsigned char *buf,
                     size_t count, off_t offset) {
    if (count < BLOCK_SIZE)
        return handle_fallback_write(type, fd, buf, count, offset);

    if (type == WRITE)
        if ((offset = lseek(fd, 0, SEEK_CUR)) < 0 || offset % BLOCK_SIZE != 0)
            return handle_fallback_write(type, fd, buf, count, offset);

    char path[4096] = {0};
    char fd_link[4096] = {0};
    sprintf(fd_link, "/proc/self/fd/%d", fd);
    if (!readlink(fd_link, path, 4095)) {
        fprintf(
            stderr,
            "libwritededuper: couldn't readlink on file descriptor %d: %m\n",
            fd);
        return handle_fallback_write(type, fd, buf, count, offset);
    };

    struct HashEntry *hash_entry;
    ssize_t written, total_written = 0;
    unsigned char block_buf[BLOCK_SIZE];

    for (ssize_t block_offset = 0; (block_offset + BLOCK_SIZE) <= count;
         block_offset += BLOCK_SIZE) {
        memcpy(block_buf, &buf[block_offset], BLOCK_SIZE);
        uint32_t hash = calculate_crc32c(0, block_buf, BLOCK_SIZE);

        if ((hash_entry = hash_table[hash]) == NULL) {
        fallback_write:
            if ((written = handle_fallback_write(type, fd, block_buf,
                                                 BLOCK_SIZE, offset)) < 0) {
                fprintf(stderr,
                        "libwritededuper: couldn't write to file descriptor "
                        "%d: %m\n",
                        fd);
                return -1;
            };

            hash_entry = malloc(sizeof(struct HashEntry));
            strlcpy(hash_entry->path, path, 4096);
            hash_entry->offset = offset;
            hash_table[hash] = hash_entry;

            offset += BLOCK_SIZE;
        } else {
            if ((fcntl(fd, F_GETFL) & O_APPEND) == O_APPEND)
                goto fallback_write;

            int in_fd;
            if ((in_fd = get_working_fd(hash_entry->path)) < 0)
                goto fallback_write;

            unsigned char in_buf[BLOCK_SIZE];
            if ((libc_pread)(in_fd, in_buf, BLOCK_SIZE, hash_entry->offset) <
                BLOCK_SIZE)
                goto fallback_write;
            if (memcmp(block_buf, in_buf, BLOCK_SIZE) != 0)
                goto fallback_write;

            if ((written = copy_file_range(in_fd, &hash_entry->offset, fd,
                                           &offset, BLOCK_SIZE, 0)) < 0) {
                goto fallback_write;
            }
            if (!lseek(fd, written, SEEK_CUR)) {
                fprintf(stderr,
                        "libwritededuper: couldn't lseek %ld bytes on file "
                        "descriptor %d: %m\n",
                        written, fd);
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

ssize_t handle_read(enum Operation type, int fd, unsigned char *buf,
                    size_t count, off_t offset) {
    if (count < BLOCK_SIZE)
        return handle_fallback_read(type, fd, buf, count, offset);

    if (type == READ)
        if ((offset = lseek(fd, 0, SEEK_CUR)) < 0 || offset % BLOCK_SIZE != 0)
            return handle_fallback_read(type, fd, buf, count, offset);

    char path[4096] = {0};
    char fd_link[4096] = {0};
    sprintf(fd_link, "/proc/self/fd/%d", fd);
    if (!readlink(fd_link, path, 4095)) {
        fprintf(
            stderr,
            "libwritededuper: couldn't readlink on file descriptor %d: %m\n",
            fd);
        return handle_fallback_read(type, fd, buf, count, offset);
    };

    ssize_t s_count;
    if ((s_count = handle_fallback_read(type, fd, buf, count, offset)) < 0)
        return s_count;

    struct HashEntry *hash_entry;
    unsigned char block_buf[BLOCK_SIZE];

    for (ssize_t block_offset = 0; (block_offset + BLOCK_SIZE) <= s_count;
         block_offset += BLOCK_SIZE) {
        memcpy(block_buf, &buf[block_offset], BLOCK_SIZE);
        uint32_t hash = calculate_crc32c(0, block_buf, BLOCK_SIZE);

        hash_entry = malloc(sizeof(struct HashEntry));
        strlcpy(hash_entry->path, path, 4096);
        hash_entry->offset = offset;
        hash_table[hash] = hash_entry;

        offset += BLOCK_SIZE;
    };

    return s_count;
}

ssize_t write(int fd, const void *buf, size_t count) {
    if (!libwritededuper_ready)
        libwritededuper_init();

    return handle_write(WRITE, fd, buf, count, -1);
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
    if (!libwritededuper_ready)
        libwritededuper_init();

    return handle_write(PWRITE, fd, buf, count, offset);
}

ssize_t read(int fd, void *buf, size_t count) {
    if (!libwritededuper_ready)
        libwritededuper_init();

    return handle_read(READ, fd, buf, count, -1);
}

ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
    if (!libwritededuper_ready)
        libwritededuper_init();

    return handle_read(PREAD, fd, buf, count, offset);
}
