#include "common.h"

#include <string.h>

int
seq_after(uint16_t a, uint16_t b)
{
    return a != b && (uint16_t)(a - b) <= 0x8000u;
}

int
seq_before(uint16_t a, uint16_t b)
{
    return seq_after(b, a);
}

uint32_t
crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (unsigned j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (UINT32_C(0xEDB88320) & (0u - (crc & 1)));
        }
    }
    return ~crc;
}

static size_t
body_bytes(uint8_t type, uint8_t len)
{
    size_t nb = type == DATA1 ? len - PART : len;
    return nb > PART ? PART : nb;
}

size_t
pack(uint8_t out[FRAME_MAX], const struct frame *f)
{
    size_t n = FRAME_HDR;
    out[0] = f->type;
    out[1] = f->flags;
    memcpy(out + 2, &f->src, 4);
    memcpy(out + 6, &f->seq, 2);
    if (f->type != ACK && f->type != HELLO) {
        size_t nb = body_bytes(f->type, f->len);
        out[n++] = f->len;
        memcpy(out + n, f->data, nb);
        n += nb;
    }
    if (f->flags & PIGGY) {
        memcpy(out + n, &f->target, 4);
        n += 4;
        memcpy(out + n, &f->ack_seq, 2);
        out[n + 2] = f->bits;
        n += 3;
    }
    uint32_t crc = crc32(out, n);
    memcpy(out + n, &crc, 4);
    return n + 4;
}

int
unpack(struct frame *f, const uint8_t *p, size_t n)
{
    if (n < FRAME_HDR + 4 || n > FRAME_MAX || p[0] < HELLO || p[0] > ACK) {
        return 0;
    }
    uint32_t crc;
    memcpy(&crc, p + n - 4, 4);
    if (crc != crc32(p, n - 4)) {
        return 0;
    }
    if (p[1] & ~(PIGGY | TX_PENDING)) {
        return 0;
    }
    memset(f, 0, sizeof *f);
    f->type = p[0];
    f->flags = p[1];
    memcpy(&f->src, p + 2, 4);
    memcpy(&f->seq, p + 6, 2);
    if (!f->src) {
        return 0;
    }
    size_t tail = 0;
    if (f->flags & PIGGY) {
        tail = ACK_TRAILER;
        const uint8_t *a = p + n - 4 - tail;
        memcpy(&f->target, a, 4);
        a += 4;
        memcpy(&f->ack_seq, a, 2);
        f->bits = a[2];
        if (f->bits & ~(3u | ACK_COMPLETE)) {
            return 0;
        }
    }
    if (f->type == ACK) {
        return (f->flags & PIGGY) && !f->seq && n == FRAME_HDR + tail + 4;
    }
    if (f->type == HELLO) {
        return n == FRAME_HDR + tail + 4;
    }
    f->len = p[FRAME_HDR];
    if (!f->len || f->len > LINE_BYTES) {
        return 0;
    }
    if (f->type != DATA0 && f->len <= PART) {
        return 0;
    }
    size_t nb = body_bytes(f->type, f->len);
    if (n != FRAME_HDR + 1 + nb + tail + 4) {
        return 0;
    }
    memcpy(f->data, p + FRAME_HDR + 1, nb);
    return 1;
}

unsigned
all_bits(size_t len)
{
    return len > PART ? 3 : 1;
}
