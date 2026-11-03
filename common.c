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

uint64_t
mix(uint64_t x)
{
    x += UINT64_C(0x9E3779B97F4A7C15);
    x = (x ^ (x >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94D049BB133111EB);
    return x ^ (x >> 31);
}

int
text_valid(const void *v, size_t n)
{
    const uint8_t *p = v;
    for (size_t i = 0; i < n; i++) {
        if (p[i] < 32 || p[i] == 127) {
            return 0;
        }
    }
    return 1;
}
