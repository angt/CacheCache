#ifndef COMMON_H
#define COMMON_H

#define _GNU_SOURCE

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <termios.h>

#define SYMBOL_NS      UINT64_C(1000000)
#define SLOT_NS        (600 * SYMBOL_NS)
#define TAIL_NS        (40 * SYMBOL_NS)
#define SYNC           0x1FFEu
#define FRAME_AIR(n)   ((23 + 8 * (n)) * SYMBOL_NS)
#define PART           41
#define LINE_BYTES     (2 * PART)
#define QUEUE_MAX      8
#define PEER_MAX       8
#define OUT_MAX        64
#define OUT_TEXT       1024
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
#define RX_WAIT_TAIL   UINT64_C(20000)
#define TX_BACKOFF     8
#define BUSY_SYMBOLS   8
#define BUSY_RATIO     1.25
#define CONTEND_NS     (2 * (TX_BACKOFF + BUSY_SYMBOLS) * SYMBOL_NS)
#define EOF_ACK_SLOTS  4
#define PROBE_BYTES    (128u << 20)
#define SWEEP_BYTES    (64u << 20)
#define SWEEP_MAX      4

_Static_assert(SYMBOL_NS + FRAME_AIR(FRAME_MAX) + TAIL_NS < SLOT_NS,
               "a complete frame must fit in its slot");
_Static_assert(CONTEND_NS + (TX_BACKOFF + BUSY_SYMBOLS + 1) * SYMBOL_NS +
               FRAME_AIR(FRAME_MAX) + TAIL_NS <= SLOT_NS,
               "a worst-case fallback attempt must fit in its slot");
_Static_assert(LINE_BYTES <= UINT8_MAX, "the line length must fit in its wire byte");
_Static_assert(QUEUE_MAX + 2 < 32768, "the in-flight window must fit inside half the sequence ring");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "signal flag must be lock-free");

enum { HELLO = 0xC1, DATA0, DATA1, PARITY, ACK };

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

struct message {
    uint16_t seq;
    uint8_t len;
    unsigned sent, round;
    char data[LINE_BYTES];
    uint64_t start, retry;
    size_t n;
    int live;
    struct {
        uint32_t id;
        unsigned bits;
    } to[PEER_MAX];
};

struct chat {
    pthread_mutex_t mx;
    uint32_t id;
    uint32_t ack_src;
    uint64_t hello;
    uint16_t seq, ack_event;
    struct remote peers[PEER_MAX];
    struct message queue[QUEUE_MAX];
    struct {
        uint32_t id;
        uint32_t next, done;
        uint8_t len;
    } gone[PEER_MAX];
    size_t gone_head, head, count;
    int full;
    uint64_t sent, recv, conf, fail, miss;
};

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

struct rx {
    uint32_t hist;
    uint64_t next;
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
uint64_t mix(uint64_t x);

int seq_after(uint16_t a, uint16_t b);
int seq_before(uint16_t a, uint16_t b);
size_t pack(uint8_t out[FRAME_MAX], const struct frame *f);
int unpack(struct frame *f, const uint8_t *p, size_t n);
unsigned all_bits(size_t len);
int text_valid(const void *v, size_t n);

struct message *pending(struct chat *c);
unsigned missing(const struct message *m);
int enqueue(struct chat *c, const char *line, size_t len);
void tick(struct chat *c, uint64_t slot);

size_t live_peers(const struct chat *c);
struct remote *touch(struct chat *c, uint32_t id, uint64_t slot);
void arm_ack(struct remote *r, uint16_t seq, unsigned bits, uint64_t slot);

struct remote *receive(struct chat *c, const struct frame *f, uint64_t slot);
int make_frame(struct chat *c, uint64_t slot, struct frame *f, int ack_now);
void sent_frame(struct chat *c, const struct frame *f, uint64_t slot, int complete);

void __attribute__((format(printf, 1, 2))) con_post(const char *fmt, ...);
void con_show(void);

void con_key(struct chat *c, unsigned char ch);
void con_open(struct chat *c);
void con_close(void);

void sweep_set(int on, uint64_t until);
void modem_open(void);
void modem_close(void);

int hear(struct frame *f, uint64_t quiet, uint64_t deadline);

int emit(const struct frame *f, struct frame *incoming, uint64_t slot);

uint32_t crc32(const uint8_t *p, size_t n);

#endif
