#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "hashmap/hashmap.h"

#define GC_TRIGGER 1000
#define GC_MAX_AGE 1

struct WorkingFd {
    char *path;
    int fd;
    unsigned long atime;
};

struct hashmap *working_fds;

uint64_t working_fd_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    const struct WorkingFd *working_fd = item;
    return hashmap_sip(working_fd->path, strlen(working_fd->path), seed0,
                       seed1);
}

int working_fd_compare(const void *a, const void *b, void *data) {
    const struct WorkingFd *aa = a;
    const struct WorkingFd *bb = b;
    return strcmp(aa->path, bb->path);
}

int get_working_fd(char *path) {
    unsigned long current_time = time(NULL);

    if (hashmap_count(working_fds) >= GC_TRIGGER) {
        size_t iter = 0;
        void *item;
        while (hashmap_iter(working_fds, &iter, &item)) {
            const struct WorkingFd *old_fd = item;
            if (current_time - old_fd->atime > GC_MAX_AGE)
                hashmap_delete(working_fds, old_fd);
        }
    }

    const struct WorkingFd *working_fd =
        hashmap_get(working_fds, &(struct WorkingFd){.path = path});

    if (!working_fd) {
        int fd;
        if ((fd = open(path, O_RDONLY)) < 0)
            return fd;
        struct WorkingFd *new_working_fd =
            &(struct WorkingFd){.path = path, .fd = fd, .atime = current_time};
        hashmap_set(working_fds, new_working_fd);
        return fd;
    }

    struct WorkingFd *new_working_fd = &(struct WorkingFd){
        .path = working_fd->path, .fd = working_fd->fd, .atime = current_time};
    hashmap_set(working_fds, working_fd);
    return working_fd->fd;
}
