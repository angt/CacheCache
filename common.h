#ifndef COMMON_H
#define COMMON_H

#define _GNU_SOURCE

#include "protocol.h"

#include <pthread.h>
#include <stdatomic.h>
#include <termios.h>

#define OUT_MAX        64
#define OUT_TEXT       1024
#define RX_WAIT_TAIL   UINT64_C(20000)
#define EOF_ACK_SLOTS  4
#define PROBE_BYTES    (128u << 20)
#define SWEEP_BYTES    (64u << 20)
#define SWEEP_MAX      4

_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "signal flag must be lock-free");

struct con {
    struct termios sv;
    int raw;
    unsigned rows, columns;
    size_t nover, nedit;
    char edit[LINE_BYTES + 1];
    _Atomic int eof, req, done;
    pthread_mutex_t mx;
    char out[OUT_MAX][OUT_TEXT];
    size_t head, count;
    int wake[2];
    pthread_t tid;
};

struct mdm {
    volatile uint32_t *ring;
    uint32_t cur;
    struct {
        pthread_t tid;
        char *buf;
    } wk[SWEEP_MAX];
    _Atomic uint64_t until;
    _Atomic int frame, halt;
    struct rx rx;
    pthread_mutex_t mx;
    pthread_cond_t cv;
};

struct cnt {
    uint64_t tx_bad, tx_ok, rx_bad, rx, sync, late;
};

struct state {
    _Atomic int stop;
    int dbg;
    struct chat chat;
    struct con con;
    struct mdm mdm;
    struct cnt cnt;
};

extern struct state cc;

void die(const char *what, int err);
void __attribute__((format(printf, 1, 2))) dbg(const char *fmt, ...);
uint64_t now(void);

void con_show(void);
void con_input(struct chat *c);
void con_open(void);
void con_close(void);

void sweep_set(int on, uint64_t until);
void modem_open(void);
void modem_close(void);
int hear(struct frame *f, uint64_t quiet, uint64_t deadline);
int emit(const struct frame *f, struct frame *incoming, uint64_t slot);

#endif
