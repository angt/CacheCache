#include "common.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

void
sweep_set(int on, uint64_t until)
{
    atomic_store(&cc.mdm.until, on ? until : 0);
}

static void *
sweep(void *arg)
{
    char *buf = arg;
    size_t off = 0;
    while (!atomic_load(&cc.mdm.halt)) {
        uint64_t until = atomic_load(&cc.mdm.until);
        if (until) {
            if (now() >= until) {
                usleep(20);
                continue;
            }
            for (size_t k = 0; k < 8192; k++) {
                if (!(k & 255) && k && now() >= until) {
                    break;
                }
                (void)*(volatile uint64_t *)(buf + off);
                off += 64;
                if (off >= SWEEP_BYTES) {
                    off = 0;
                }
            }
        } else if (!atomic_load(&cc.mdm.frame)) {
            pthread_mutex_lock(&cc.mdm.mx);
            while (!atomic_load(&cc.mdm.until) &&
                   !atomic_load(&cc.mdm.frame) &&
                   !atomic_load(&cc.mdm.halt)) {
                pthread_cond_wait(&cc.mdm.cv, &cc.mdm.mx);
            }
            pthread_mutex_unlock(&cc.mdm.mx);
        } else {
            usleep(50);
        }
    }
    return NULL;
}

static void *
allocate(size_t size)
{
    void *p;
    int err = posix_memalign(&p, 1u << 21, size);
    if (err) {
        die("posix_memalign", err);
    }
#ifdef MADV_HUGEPAGE
    madvise(p, size, MADV_HUGEPAGE);
#endif
    memset(p, 0, size);
    return p;
}

void
modem_open(void)
{
    size_t lines = PROBE_BYTES / 64;
    uint32_t *order = malloc(lines * sizeof *order);
    if (!order) {
        die("malloc", errno);
    }
    cc.mdm.ring = allocate(PROBE_BYTES);
    cc.mdm.cur = air_ring(cc.mdm.ring, order, lines, cc.chat.id);
    free(order);
    for (size_t i = 0; i < SWEEP_MAX; i++) {
        cc.mdm.wk[i].buf = allocate(SWEEP_BYTES);
        int err = pthread_create(&cc.mdm.wk[i].tid, NULL, sweep, cc.mdm.wk[i].buf);
        if (err) {
            die("pthread_create", err);
        }
    }
}

void
modem_close(void)
{
    pthread_mutex_lock(&cc.mdm.mx);
    atomic_store(&cc.mdm.halt, 1);
    sweep_set(0, 0);
    atomic_store(&cc.mdm.frame, 0);
    pthread_cond_broadcast(&cc.mdm.cv);
    pthread_mutex_unlock(&cc.mdm.mx);
    for (size_t i = 0; i < SWEEP_MAX; i++) {
        pthread_join(cc.mdm.wk[i].tid, NULL);
        free(cc.mdm.wk[i].buf);
    }
    free((void *)cc.mdm.ring);
}
