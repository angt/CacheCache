#include "protocol.h"

size_t
air_encode(uint8_t *out, const uint8_t *in, size_t n)
{
    size_t nb = 0;
    out[nb++] = 0;
    for (unsigned i = 0; i < AIR_BIT_SYNC; i++) {
        out[nb++] = (SYNC >> i) & 1;
    }
    for (unsigned i = 0; i < AIR_BIT_LENGTH; i++) {
        out[nb++] = (n >> i) & 1;
    }
    for (size_t i = 0; i < n; i++) {
        for (unsigned j = 0; j < 8; j++) {
            out[nb++] = (in[i] >> j) & 1;
        }
    }
    return nb;
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
        ring[order[i] * AIR_RING_STRIDE] = order[(i + 1) % lines] * AIR_RING_STRIDE;
    }
    return start % lines * AIR_RING_STRIDE;
}

unsigned
air_symbol_busy(unsigned bit, unsigned half)
{
    return bit ? half == 0 : half == 1;
}

double
air_load_ratio(double first, double second)
{
    return first > second ? first / second : second / first;
}

int
air_channel_clear(const double *w, size_t n)
{
    size_t m = n < BUSY_SYMBOLS ? n : BUSY_SYMBOLS;
    if (!m || w[m - 1] > BUSY_RATIO) {
        return 0;
    }
    double copy[BUSY_SYMBOLS];
    for (size_t i = 0; i < m; i++) copy[i] = w[i];
    return air_median(copy, m) <= BUSY_RATIO;
}

enum air_bit_phase
air_bit_phase(size_t at)
{
    if (at == 0) {
        return AIR_PHASE_LEADER;
    }
    if (at < AIR_BIT_HEADER - AIR_BIT_LENGTH) {
        return AIR_PHASE_SYNC;
    }
    if (at < AIR_BIT_HEADER) {
        return AIR_PHASE_LENGTH;
    }
    return AIR_PHASE_PAYLOAD;
}

void
air_rx_reset(struct rx *r)
{
    r->hist = 0;
    r->acc = r->value = r->length = 0;
    r->count = 0;
    r->synced = r->length_done = 0;
    r->bit = 0;
}

int
air_recv(struct rx *r, struct frame *f, double first, double second)
{
    unsigned bit = first > second;
    r->bit = bit;
    if (!r->synced) {
        r->hist = (r->hist << 1 | (bit & 1)) & 0x3FFF;
        if (r->hist != SYNC) {
            return AIR_RX_WAIT;
        }
        r->synced = 1;
        r->acc = r->value = 0;
        return AIR_RX_SYNC;
    }
    r->value |= (unsigned)(bit & 1) << r->acc++;
    if (r->acc < 8) {
        return AIR_RX_WAIT;
    }
    r->acc = 0;
    unsigned byte = r->value;
    r->value = 0;
    if (!r->length_done) {
        r->length_done = 1;
        r->length = byte;
        r->count = 0;
        if (r->length < FRAME_HDR + 4 || r->length > FRAME_MAX) {
            return AIR_RX_BAD_LENGTH;
        }
        return AIR_RX_LENGTH;
    }
    if (r->count >= FRAME_MAX) {
        return AIR_RX_OVERFLOW;
    }
    r->bytes[r->count++] = (uint8_t)byte;
    if (r->count != r->length) {
        return AIR_RX_BYTE;
    }
    int ok = unpack(f, r->bytes, r->count);
    r->hist = 0;
    r->acc = r->value = 0;
    r->synced = r->length_done = 0;
    return ok ? AIR_RX_FRAME : AIR_RX_REJECT;
}
