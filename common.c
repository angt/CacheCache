#include "common.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

struct state cc = {
    .chat.mx = PTHREAD_MUTEX_INITIALIZER,
    .con.mx = PTHREAD_MUTEX_INITIALIZER,
    .mdm = {
        .mx = PTHREAD_MUTEX_INITIALIZER,
        .cv = PTHREAD_COND_INITIALIZER
    },
};

void
die(const char *what, int err)
{
    errno = err;
    perror(what);
    exit(1);
}

void
dbg(const char *fmt, ...)
{
    if (!cc.dbg) {
        return;
    }
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    dprintf(STDERR_FILENO, "[%.2f] %s\n", now() / 1e9, msg);
}

uint64_t
now(void)
{
    struct timespec t = {0};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * UINT64_C(1000000000) + t.tv_nsec;
}
