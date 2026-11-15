#include "common.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static uint32_t
probe_read32(const volatile uint32_t *p)
{
    return *p;
}

static void
probe_read64(const volatile void *p)
{
    (void)*(const volatile uint64_t *)p;
}

static void
spin_until(uint64_t end)
{
    while (!cc.stop && now() < end);
}

static uint64_t
next_symbol(void)
{
    return (now() / SYMBOL_NS + 1) * SYMBOL_NS;
}

static void
idle_until(uint64_t end)
{
    uint64_t t = now();
    if (end > t + 2 * RX_WAIT_TAIL) {
        uint64_t ns = end - RX_WAIT_TAIL - t;
        struct timespec d = {
            ns / 1000000000,
            ns % 1000000000,
        };
        nanosleep(&d, NULL);
    }
    spin_until(end);
}

static int
rx_half(uint64_t start, uint64_t end, double *out)
{
    double samples[RX_SAMPLE_K];
    size_t count = 0;
    uint32_t cur = cc.mdm.cur;
    uint64_t span = (end - start) / RX_SAMPLE_K;
    for (size_t i = 0; i < RX_SAMPLE_K && !cc.stop; i++) {
        uint64_t w0 = start + i * span;
        uint64_t w1 = i == RX_SAMPLE_K - 1 ? end : w0 + span;
        idle_until(w0);
        if (now() >= w1) {
            continue;
        }
        uint64_t t0 = now();
        size_t hops = 0;
        uint64_t elapsed = t0;
        while (hops < RX_BATCH_HOPS) {
            for (size_t j = 0; j < 32; j++) {
                cur = probe_read32(&cc.mdm.ring[cur]);
            }
            hops += 32;
            elapsed = now();
            if (elapsed >= w1) {
                break;
            }
        }
        samples[count++] = (double)(elapsed - t0) / hops;
    }
    cc.mdm.cur = cur;
    if (count < 2) {
        return 0;
    }
    *out = air_median(samples, count);
    return 1;
}

static int
rx_symbol(uint64_t at, double *first, double *second, double *ratio)
{
    if (ratio) {
        *ratio = BUSY_RATIO + 1;
    }
    if (now() > at + SYMBOL_NS / 8) {
        cc.cnt.late++;
    }
    int ok1 = rx_half(at, at + SYMBOL_NS / 2, first);
    int ok2 = rx_half(at + SYMBOL_NS / 2, at + SYMBOL_NS, second);
    spin_until(at + SYMBOL_NS);
    if (!ok1 || !ok2) {
        return 0;
    }
    if (ratio && *first > 0 && *second > 0) {
        *ratio = air_load_ratio(*first, *second);
    }
    return 1;
}

static void
resync(void)
{
    if (now() % SYMBOL_NS > SYMBOL_NS / 4) {
        spin_until(next_symbol());
    }
}

static int
receive_air(struct frame *f, uint64_t quiet, uint64_t deadline, uint64_t sense, double w[BUSY_SYMBOLS])
{
    int synced = 0;
    struct rx *r = &cc.mdm.rx;
    uint64_t at = r->next;
    if (!at || at < now() / SYMBOL_NS * SYMBOL_NS || at > next_symbol()) {
        air_rx_reset(r);
        resync();
        at = now() / SYMBOL_NS * SYMBOL_NS;
    }
    while (!cc.stop && now() < quiet) {
        double ratio, first, second;
        int ok = rx_symbol(at, &first, &second, &ratio);
        r->next = at + SYMBOL_NS;
        if (w && at >= sense && at < sense + BUSY_SYMBOLS * SYMBOL_NS) {
            w[(at - sense) / SYMBOL_NS] = ratio;
        }
        at = r->next;
        if (!ok) {
            int in_frame = r->synced;
            int in_payload = r->length_done;
            air_rx_reset(r);
            if (in_frame) {
                cc.cnt.rx_bad++;
                dbg(in_payload ? "sync; frame rejected" : "frame cut off");
            }
            if (!in_frame || in_payload) {
                resync();
                at = now() / SYMBOL_NS * SYMBOL_NS;
                r->next = at;
            }
            continue;
        }
        switch (air_recv(r, f, first, second)) {
        case AIR_RX_WAIT:
        case AIR_RX_BYTE:
            break;
        case AIR_RX_SYNC:
            quiet = deadline;
            synced = 1;
            cc.cnt.sync++;
            break;
        case AIR_RX_LENGTH:
            if (at + (uint64_t)r->length * 8 * SYMBOL_NS > deadline) {
                cc.cnt.rx_bad++;
                dbg("frame cut off");
                air_rx_reset(r);
            }
            break;
        case AIR_RX_BAD_LENGTH:
        case AIR_RX_OVERFLOW:
            cc.cnt.rx_bad++;
            dbg("frame cut off");
            air_rx_reset(r);
            break;
        case AIR_RX_REJECT:
            cc.cnt.rx_bad++;
            dbg("sync; frame rejected");
            break;
        case AIR_RX_FRAME:
            cc.cnt.rx++;
            dbg("sync; frame ok n=%zu", r->count);
            return 1;
        }
    }
    return synced ? -1 : 0;
}

int
hear(struct frame *f, uint64_t quiet, uint64_t deadline)
{
    return receive_air(f, quiet, deadline, 0, NULL) > 0;
}

static int
emit_bits(const uint8_t *bytes, size_t n, uint64_t at, uint64_t deadline)
{
    uint8_t bits[AIR_BIT_HEADER + 8 * FRAME_MAX];
    size_t nb = air_encode(bits, bytes, n);
    for (size_t i = 0; i < nb; i++, at += SYMBOL_NS) {
        if (cc.stop || at > deadline || SYMBOL_NS > deadline - at) {
            return 0;
        }
        spin_until(at);
        if (now() >= at + SYMBOL_NS) {
            sweep_set(0, 0);
            return 0;
        }
        sweep_set(air_symbol_busy(bits[i], 0), at + SYMBOL_NS / 2);
        spin_until(at + SYMBOL_NS / 2);
        sweep_set(air_symbol_busy(bits[i], 1), at + SYMBOL_NS);
        spin_until(at + SYMBOL_NS);
        sweep_set(0, 0);
    }
    return 1;
}


static int
clear_to_send(uint32_t id, uint64_t slot, struct frame *incoming)
{
    uint64_t sense = next_symbol() + ((mix(id ^ mix(slot)) >> 3) % TX_BACKOFF) * SYMBOL_NS;
    double w[BUSY_SYMBOLS];
    for (size_t i = 0; i < BUSY_SYMBOLS; i++) {
        w[i] = BUSY_RATIO + 1;
    }
    int received = receive_air(incoming, sense + BUSY_SYMBOLS * SYMBOL_NS, (slot + 1) * SLOT_NS, sense, w);
    if (received > 0) {
        return 2;
    }
    if (received < 0) {
        return 0;
    }
    return air_channel_clear(w, BUSY_SYMBOLS);
}

static int
frame_fits(size_t n, uint64_t at, uint64_t deadline)
{
    return at <= deadline && FRAME_AIR(n) <= deadline - at;
}

int
emit(const struct frame *f, struct frame *incoming, uint64_t slot)
{
    uint8_t bytes[FRAME_MAX];
    size_t n = pack(bytes, f);
    uint64_t deadline = (slot + 1) * SLOT_NS - TAIL_NS;
    if (!frame_fits(n, next_symbol(), deadline)) {
        return -1;
    }
    int access = clear_to_send(f->src, slot, incoming);
    if (access == 2) {
        dbg("defer: frame received");
        return 2;
    }
    if (!access) {
        dbg("defer: tone on channel");
        return -1;
    }
    cc.mdm.rx = (struct rx){0};
    pthread_mutex_lock(&cc.mdm.mx);
    atomic_store(&cc.mdm.frame, 1);
    pthread_cond_broadcast(&cc.mdm.cv);
    uint64_t at = next_symbol();
    pthread_mutex_unlock(&cc.mdm.mx);
    int ok = frame_fits(n, at, deadline) ? emit_bits(bytes, n, at, deadline) : -1;
    pthread_mutex_lock(&cc.mdm.mx);
    atomic_store(&cc.mdm.frame, 0);
    pthread_mutex_unlock(&cc.mdm.mx);
    cc.cnt.tx_bad += ok == 0;
    cc.cnt.tx_ok += ok > 0;
    dbg("frame %s n=%zu", ok > 0 ? "sent" : ok < 0 ? "deferred" : "aborted", n);
    return ok;
}

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
                probe_read64(buf + off);
                off += CACHE_LINE_BYTES;
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
    size_t lines = PROBE_BYTES / CACHE_LINE_BYTES;
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
