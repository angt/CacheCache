#include "common.h"

#include <inttypes.h>
#include <string.h>

struct message *
pending(struct chat *c)
{
    return c->count ? &c->queue[c->head] : NULL;
}

static int
add_target(struct message *m, uint32_t id)
{
    for (size_t i = 0; i < m->n; i++) {
        if (m->to[i].id == id) {
            return 0;
        }
    }
    if (m->n == PEER_MAX) {
        return 0;
    }
    m->to[m->n].bits = 0;
    m->to[m->n++].id = id;
    return 1;
}

unsigned
missing(const struct message *m)
{
    unsigned all = all_bits(m->len);
    unsigned bits = m->n ? 0 : all;
    for (size_t i = 0; i < m->n; i++) {
        unsigned state = m->to[i].bits;
        if (!(state & ACK_COMPLETE) || (state & all) != all) {
            unsigned left = all & ~state;
            bits |= left ? left : all;
        }
    }
    return bits;
}

int
enqueue(struct chat *c, const char *line, size_t len)
{
    if (len < 1 || len > LINE_BYTES || c->count == QUEUE_MAX) {
        return 0;
    }
    struct message *m = &c->queue[(c->head + c->count) % QUEUE_MAX];
    memset(m, 0, sizeof *m);
    m->seq = c->seq++;
    m->len = len;
    memcpy(m->data, line, len);
    c->count++;
    return 1;
}

void
tick(struct chat *c, uint64_t slot)
{
    for (size_t i = 0; i < PEER_MAX; i++) {
        struct remote *r = &c->peers[i];
        if (r->live && slot - r->seen >= QUIET_SLOTS) {
            r->live = 0;
            r->ack = 0;
            c->full = 0;
            con_post("peer %08" PRIx32 " went quiet", r->id);
        }
    }
    struct message *m = pending(c);
    unsigned want = m ? missing(m) : 0;
    if (m && want) {
        size_t keep = 0;
        for (size_t i = 0; i < m->n; i++) {
            int here = 0;
            for (size_t j = 0; j < PEER_MAX; j++) {
                if (c->peers[j].live && c->peers[j].id == m->to[i].id) {
                    here = 1;
                }
            }
            if (here) {
                m->to[keep++] = m->to[i];
            }
        }
        m->n = keep;
        want = missing(m);
    }
    if (m && m->live && (!want || slot - m->start >= LIFE_SLOTS)) {
        if (want) {
            c->fail++;
            con_post("line #%" PRIu16 " unconfirmed (timed out)", m->seq);
        } else {
            c->conf++;
        }
        c->hello = slot;
        c->head = (c->head + 1) % QUEUE_MAX;
        c->count--;
        m = pending(c);
    }
    if (m && !m->live) {
        m->live = 1;
        m->start = m->retry = slot;
        for (size_t i = 0; i < PEER_MAX; i++) {
            if (c->peers[i].live) {
                add_target(m, c->peers[i].id);
            }
        }
    }
}

size_t
live_peers(const struct chat *c)
{
    size_t n = 0;
    for (size_t i = 0; i < PEER_MAX; i++) {
        n += c->peers[i].live;
    }
    return n;
}

static void
forget(struct chat *c, const struct remote *r)
{
    size_t use = PEER_MAX;
    for (size_t i = 0; i < PEER_MAX && use == PEER_MAX; i++) {
        if (c->gone[i].id == r->id) {
            use = i;
        }
    }
    for (size_t i = 0; i < PEER_MAX && use == PEER_MAX; i++) {
        if (!c->gone[i].id) {
            use = i;
        }
    }
    if (use == PEER_MAX) {
        use = c->gone_head;
        c->gone_head = (c->gone_head + 1) % PEER_MAX;
    }
    c->gone[use].id   = r->id;
    c->gone[use].next = r->next;
    c->gone[use].done = r->ok ? r->done : UINT32_MAX;
    c->gone[use].len  = r->ok ? r->len : 0;
}

static void
resume(struct chat *c, struct remote *r)
{
    for (size_t i = 0; i < PEER_MAX; i++) {
        if (c->gone[i].id != r->id) {
            continue;
        }
        r->known = 1;
        r->next  = c->gone[i].next;
        r->ok    = c->gone[i].done != UINT32_MAX;
        r->done  = c->gone[i].done;
        if (r->ok) {
            r->seq = r->done;
            r->len = c->gone[i].len;
        }
        c->gone[i].id = 0;
        return;
    }
}

struct remote *
touch(struct chat *c, uint32_t id, uint64_t slot)
{
    struct remote *r = NULL;
    for (size_t i = 0; i < PEER_MAX; i++) {
        struct remote *s = &c->peers[i];
        if (s->id == id) {
            r = s;
            break;
        }
        if (!s->live && (!r || s->seen < r->seen)) {
            r = s;
        }
    }
    if (!r) {
        if (!c->full) {
            con_post("peer table full; ignoring additional peers");
        }
        c->full = 1;
        return NULL;
    }
    if (r->id != id) {
        struct remote old = *r;
        c->full = 0;
        memset(r, 0, sizeof *r);
        r->id = id;
        resume(c, r);
        if (old.known) {
            forget(c, &old);
        }
    }
    if (!r->live) {
        con_post("peer %08" PRIx32 " joined", id);
    }
    r->live = 1;
    r->seen = slot;
    struct message *m = pending(c);
    if (m && m->live && add_target(m, id)) {
        m->round = 0;
        m->retry = slot + 1;
    }
    return r;
}

void
arm_ack(struct remote *r, uint16_t seq, unsigned bits, uint64_t slot)
{
    if (r->ack && seq_before(seq, r->ack_seq)) {
        return;
    }
    if (!r->ack) {
        r->ack_at = slot;
    }
    r->ack = 1;
    r->ack_seq = seq;
    r->ack_bits = bits;
}
