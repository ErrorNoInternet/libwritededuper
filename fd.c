#include "fcntl.h"
#include "hashmap.h"
#include <dirent.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

struct hashmap *working_fds;

typedef struct {
    char *path;
    int fd;
} WorkingFd;

uint64_t working_fd_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    const WorkingFd *working_fd = item;
    return hashmap_sip(working_fd->path, strlen(working_fd->path), seed0,
                       seed1);
}

int working_fd_compare(const void *a, const void *b, void *data) {
    const WorkingFd *aa = a;
    const WorkingFd *bb = b;
    return strcmp(aa->path, bb->path);
}

int get_working_fd(char *path) {
    const WorkingFd *working_fd =
        hashmap_get(working_fds, &(WorkingFd){.path = path});

    if (!working_fd) {
        int fd = open(path, O_RDWR);
        if (fd < 0)
            return fd;
        working_fd = &(WorkingFd){.path = path, .fd = fd};
        hashmap_set(working_fds, working_fd);
    }

    return working_fd->fd;
}
