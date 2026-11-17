#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define SYMBOL_NS      UINT64_C(1000000)
#define SLOT_NS        (600 * SYMBOL_NS)
#define TAIL_NS        (40 * SYMBOL_NS)
#define SYNC           0x1FFEu

#define AIR_BIT_LEADER   1
#define AIR_BIT_SYNC     14
#define AIR_BIT_LENGTH   8
#define AIR_BIT_HEADER   (AIR_BIT_LEADER + AIR_BIT_SYNC + AIR_BIT_LENGTH)

#define CACHE_LINE_BYTES 64
#define AIR_RING_STRIDE  (CACHE_LINE_BYTES / sizeof(uint32_t))
#define FRAME_AIR(n)     ((AIR_BIT_HEADER + 8 * (n)) * SYMBOL_NS)

#define PART           41
#define LINE_BYTES     (2 * PART)
#define QUEUE_MAX      8
#define PEER_MAX       8
#define FRAME_HDR      8
#define ACK_TRAILER    7
#define FRAME_MAX      (FRAME_HDR + 1 + PART + ACK_TRAILER + 4)
#define PIGGY          0x01u
#define TX_PENDING     0x02u
#define ACK_COMPLETE   0x80u
#define HELLO_SLOTS    100
#define DISCOVER_SLOTS 13
#define QUIET_SLOTS    300
#define LIFE_SLOTS     1500
#define RX_BATCH_HOPS  128
#define RX_SAMPLE_K    5
#define TX_BACKOFF     8
#define BUSY_SYMBOLS   8
#define BUSY_RATIO     1.25
#define CONTEND_NS     (2 * (TX_BACKOFF + BUSY_SYMBOLS) * SYMBOL_NS)

_Static_assert(SYMBOL_NS + FRAME_AIR(FRAME_MAX) + TAIL_NS < SLOT_NS,
               "a complete frame must fit in its slot");
_Static_assert(CONTEND_NS + (TX_BACKOFF + BUSY_SYMBOLS + 1) * SYMBOL_NS +
               FRAME_AIR(FRAME_MAX) + TAIL_NS <= SLOT_NS,
               "a worst-case fallback attempt must fit in its slot");
_Static_assert(LINE_BYTES <= UINT8_MAX, "the line length must fit in its wire byte");
_Static_assert(QUEUE_MAX + 2 < 32768, "the in-flight window must fit inside half the sequence ring");

enum air_bit_phase {
    AIR_PHASE_LEADER,
    AIR_PHASE_SYNC,
    AIR_PHASE_LENGTH,
    AIR_PHASE_PAYLOAD,
};

enum { HELLO = 0xC1, DATA0, DATA1, PARITY, ACK };

enum {
    AIR_RX_WAIT = 0,
    AIR_RX_SYNC = 1,
    AIR_RX_LENGTH = 2,
    AIR_RX_BYTE = 3,
    AIR_RX_FRAME = 4,
    AIR_RX_BAD_LENGTH = -1,
    AIR_RX_OVERFLOW = -2,
    AIR_RX_REJECT = -3,
};

struct frame {
    uint32_t src, target;
    uint16_t seq, ack_seq;
    uint8_t type, flags, len, bits, data[PART];
};

struct remote {
    uint32_t id;
    uint64_t seen, ack_at;
    uint16_t next, seq, done, ack_seq;
    uint8_t len;
    unsigned got, ack_bits;
    char data[LINE_BYTES], parity[PART];
    int live, known, ok, par, ack, tx_pending;
};

struct target {
    uint32_t id;
    unsigned bits;
};

struct message {
    uint16_t seq;
    uint8_t len;
    unsigned sent, round;
    char data[LINE_BYTES];
    uint64_t start, retry;
    size_t n;
    int live;
    struct target to[PEER_MAX];
};

struct chat {
    uint32_t id;
    uint32_t ack_src;
    uint64_t hello, sent_slot, fast_slot;
    uint16_t seq, ack_event;
    struct remote peers[PEER_MAX];
    struct message queue[QUEUE_MAX];
    struct {
        uint32_t id;
        uint32_t next, done;
        uint8_t len;
    } gone[PEER_MAX];
    struct target quiet[PEER_MAX * (2 + LIFE_SLOTS / QUIET_SLOTS)];
    size_t nquiet;
    size_t gone_head, head, count;
    int full;
    uint64_t sent, recv, conf, fail, miss;
};

struct rx {
    uint64_t next;
    uint32_t hist;
    unsigned acc, value, length;
    size_t count;
    int synced, length_done;
    unsigned bit;
    uint8_t bytes[FRAME_MAX];
};

size_t air_encode(uint8_t *out, const uint8_t *in, size_t n);
double air_median(double *values, size_t count);
uint32_t air_ring(volatile uint32_t *ring, uint32_t *order, size_t lines, uint32_t start);
enum air_bit_phase air_bit_phase(size_t at);
unsigned air_symbol_busy(unsigned bit, unsigned half);
double air_load_ratio(double first, double second);
int air_channel_clear(const double *w, size_t n);
void air_rx_reset(struct rx *r);
int air_recv(struct rx *r, struct frame *f, double first, double second);

int seq_after(uint16_t a, uint16_t b);
int seq_before(uint16_t a, uint16_t b);
size_t pack(uint8_t out[FRAME_MAX], const struct frame *f);
int unpack(struct frame *f, const uint8_t *p, size_t n);
unsigned all_bits(size_t len);
int text_valid(const void *v, size_t n);
uint32_t crc32(const uint8_t *p, size_t n);

void chat_init(struct chat *c, uint32_t id);
struct message *pending(struct chat *c);
unsigned missing(const struct message *m);
int enqueue(struct chat *c, const char *line, size_t len);
void tick(struct chat *c, uint64_t slot);
size_t live_peers(const struct chat *c);
struct remote *touch(struct chat *c, uint32_t id, uint64_t slot);
void arm_ack(struct remote *r, uint16_t seq, unsigned bits, uint64_t slot);

struct remote *receive(struct chat *c, const struct frame *f, uint64_t slot);
int make_frame(struct chat *c, uint64_t slot, struct frame *f, int ack_now);
int select_frame(struct chat *c, uint64_t slot, struct frame *f, int ack_only,
                 int *response);
void sent_frame(struct chat *c, const struct frame *f, uint64_t slot, int complete);

void __attribute__((format(printf, 1, 2))) con_post(const char *fmt, ...);

static inline uint64_t
mix(uint64_t x)
{
    x += UINT64_C(0x9E3779B97F4A7C15);
    x = (x ^ (x >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94D049BB133111EB);
    return x ^ (x >> 31);
}

#endif
