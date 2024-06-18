#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hiredis/hiredis.h"

redisContext *c;

void hashtable_set(unsigned int key, char *path, off_t offset) {
    char value[PATH_MAX + 11] = {0};
    unsigned long copied = strlcpy(value, path, PATH_MAX - 1);
    sprintf(&value[copied + 1], "%ld", offset);

    redisReply *reply = redisCommand(c, "SET %u %b", key, value);
    freeReplyObject(reply);
}

void hashtable_init() {
    char *host;
    if (!(host = getenv("LIBWRITEDEDUPER_REDIS_HOST")))
        host = "127.0.0.1";

    int is_unix = 0, port;
    char *str_port;
    if ((str_port = getenv("LIBWRITEDEDUPER_REDIS_PORT")))
        port = atoi(str_port);
    else
        is_unix = 1;

    struct timeval timeout = {1, 0};
    if (is_unix)
        c = redisConnectUnixWithTimeout(host, timeout);
    else
        c = redisConnectWithTimeout(host, port, timeout);

    if (!c || c->err) {
        if (c) {
            fprintf(stderr, "libwritededuper: redis connection error: %s\n",
                    c->errstr);
            redisFree(c);
        } else
            fprintf(stderr, "libwritededuper: redis connection error: can't "
                            "allocate redis context\n");
        exit(EXIT_FAILURE);
    }
}
