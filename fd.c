#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

char *fd_path_cache;

char *fd_path(int fd) {
    char *path = &fd_path_cache[fd];
    if (path[0] != 0)
        return path;

    char fd_path[4096] = {0};
    sprintf(fd_path, "/proc/self/fd/%d", fd);
    if (!readlink(fd_path, &fd_path_cache[fd], 4095)) {
        fprintf(stderr, "couldn't readlink on file descriptor %d: errno %d\n",
                fd, errno);
        exit(EXIT_FAILURE);
    };
    return &fd_path_cache[fd];
}
