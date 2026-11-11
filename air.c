#include "common.h"

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

static void
sort_doubles(double *values, size_t count)
{
    for (size_t i = 1; i < count; i++) {
        double value = values[i];
        size_t j = i;
        while (j && values[j - 1] > value) {
            values[j] = values[j - 1];
            j--;
        }
        values[j] = value;
    }
}

double
air_median(double *values, size_t count)
{
    sort_doubles(values, count);
    return 0.5 * (values[(count - 1) / 2] + values[count / 2]);
}

uint32_t
air_ring(volatile uint32_t *ring, uint32_t *order, size_t lines, uint32_t start)
{
    for (size_t i = 0; i < lines; i++) {
        order[i] = i;
    }
    for (size_t i = lines - 1; i > 0; i--) {
        size_t j = (uint32_t)mix(i) % (i + 1);
        uint32_t tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }
    for (size_t i = 0; i < lines; i++) {
        ring[order[i] * 16] = order[(i + 1) % lines] * 16;
    }
    return start % lines * 16;
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
                cur = cc.mdm.ring[cur];
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
rx_bit(uint64_t at, double *ratio)
{
    double a;
    double b;
    if (ratio) {
        *ratio = BUSY_RATIO + 1;
    }
    if (now() > at + SYMBOL_NS / 8) {
        cc.cnt.late++;
    }
    int first = rx_half(at, at + SYMBOL_NS / 2, &a);
    int second = rx_half(at + SYMBOL_NS / 2, at + SYMBOL_NS, &b);
    spin_until(at + SYMBOL_NS);
    if (!first || !second) {
        return -1;
    }
    if (ratio && a > 0 && b > 0) {
        *ratio = a > b ? a / b : b / a;
    }
    return a > b;
}

static int
rx_byte(uint64_t *at)
{
    int byte = 0;
    for (unsigned i = 0; i < 8 && !cc.stop; i++) {
        int b = rx_bit(*at, NULL);
        *at += SYMBOL_NS;
        if (b < 0) {
            return -1;
        }
        byte |= b << i;
    }
    return cc.stop ? -1 : byte;
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
    uint64_t at = cc.mdm.rx.next;
    if (!at || at < now() / SYMBOL_NS * SYMBOL_NS || at > next_symbol()) {
        cc.mdm.rx.hist = 0;
        resync();
        at = now() / SYMBOL_NS * SYMBOL_NS;
    }
    while (!cc.stop && now() < quiet) {
        double ratio;
        int b = rx_bit(at, &ratio);
        cc.mdm.rx.next = at + SYMBOL_NS;
        if (w && at >= sense && at < sense + BUSY_SYMBOLS * SYMBOL_NS) {
            w[(at - sense) / SYMBOL_NS] = ratio;
        }
        if (b < 0) {
            cc.mdm.rx.hist = 0;
            resync();
            at = now() / SYMBOL_NS * SYMBOL_NS;
            cc.mdm.rx.next = at;
            continue;
        }
        cc.mdm.rx.hist = (cc.mdm.rx.hist << 1 | b) & 0x3FFF;
        if (cc.mdm.rx.hist != SYNC) {
            at = cc.mdm.rx.next;
            continue;
        }
        quiet = deadline;
        synced = 1;
        cc.cnt.sync++;
        at = cc.mdm.rx.next;
        b = rx_byte(&at);
        cc.mdm.rx.next = at;
        if (b < FRAME_HDR + 4 || b > FRAME_MAX || at + b * 8 * SYMBOL_NS > deadline) {
            cc.cnt.rx_bad++;
            dbg("frame cut off");
            cc.mdm.rx.hist = 0;
            continue;
        }
        size_t n = b;
        uint8_t bytes[FRAME_MAX];
        size_t i;
        for (i = 0; i < n; i++) {
            b = rx_byte(&at);
            cc.mdm.rx.next = at;
            if (b < 0) {
                break;
            }
            bytes[i] = b;
        }
        cc.mdm.rx.hist = 0;
        if (i == n && unpack(f, bytes, n)) {
            cc.cnt.rx++;
            dbg("sync; frame ok n=%zu", n);
            return 1;
        }
        cc.cnt.rx_bad++;
        dbg("sync; frame rejected");
        if (i < n) {
            resync();
            at = now() / SYMBOL_NS * SYMBOL_NS;
            cc.mdm.rx.next = at;
        }
    }
    return synced ? -1 : 0;
}

int
hear(struct frame *f, uint64_t quiet, uint64_t deadline)
{
    return receive_air(f, quiet, deadline, 0, NULL) > 0;
}

size_t
air_encode(uint8_t *out, const uint8_t *in, size_t n)
{
    size_t nb = 0;
    out[nb++] = 0;
    for (unsigned i = 0; i < 14; i++) {
        out[nb++] = (SYNC >> i) & 1;
    }
    for (unsigned i = 0; i < 8; i++) {
        out[nb++] = (n >> i) & 1;
    }
    for (size_t i = 0; i < n; i++) {
        for (unsigned j = 0; j < 8; j++) {
            out[nb++] = (in[i] >> j) & 1;
        }
    }
    return nb;
}

static int
emit_bits(const uint8_t *bytes, size_t n, uint64_t at, uint64_t deadline)
{
    uint8_t bits[8 * FRAME_MAX + 32];
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
        sweep_set(bits[i], at + SYMBOL_NS / 2);
        spin_until(at + SYMBOL_NS / 2);
        sweep_set(!bits[i], at + SYMBOL_NS);
        spin_until(at + SYMBOL_NS);
        sweep_set(0, 0);
    }
    return 1;
}

static int
carrier_clear(double w[BUSY_SYMBOLS])
{
    if (w[BUSY_SYMBOLS - 1] > BUSY_RATIO) {
        return 0;
    }
    return air_median(w, BUSY_SYMBOLS) <= BUSY_RATIO;
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
    return carrier_clear(w);
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
