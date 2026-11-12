#include "common.h"

#include <inttypes.h>
#include <string.h>

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

struct remote *
receive(struct chat *c, const struct frame *f, uint64_t slot)
{
    if (f->src == c->id) {
        return NULL;
    }
    struct remote *r = touch(c, f->src, slot);
    if (!r) {
        return NULL;
    }
    r->tx_pending = !!(f->flags & TX_PENDING);
    if (f->flags & PIGGY) {
        struct message *m = pending(c);
        if (m && m->live && f->target == c->id && f->ack_seq == m->seq) {
            unsigned all = all_bits(m->len);
            unsigned bits = f->bits & all;
            int complete = f->bits & ACK_COMPLETE;
            if (!complete || bits == all) {
                for (size_t i = 0; i < m->n; i++) {
                    if (m->to[i].id == f->src &&
                        !(m->to[i].bits & ACK_COMPLETE) &&
                        m->to[i].bits != f->bits) {
                        m->to[i].bits = f->bits;
                        m->round = 0;
                        m->retry = slot + 1;
                    }
                }
            }
        }
    }
    if (f->type == ACK || f->type == HELLO) {
        return NULL;
    }
    if (!r->known) {
        r->known = 1;
        r->next = f->seq;
    }
    if (f->seq != r->next) {
        uint16_t back = r->next - f->seq;
        if (seq_after(f->seq, r->next) || back > QUEUE_MAX + 2) {
            c->miss += (uint16_t)(f->seq - r->next);
            uint16_t last = f->seq - 1;
            con_post("peer %08" PRIx32 ": missed lines %" PRIu16 "..%" PRIu16, r->id, r->next, last);
            r->next = f->seq;
        } else {
            if (r->ok && f->seq == r->done) {
                arm_ack(r, f->seq, all_bits(r->len) | ACK_COMPLETE, slot);
            }
            return NULL;
        }
    }
    if (!r->len || r->seq != f->seq) {
        r->seq = f->seq;
        r->len = f->len;
        r->got = 0;
        r->par = 0;
        memset(r->data, 0, sizeof r->data);
    }
    if (r->len != f->len) {
        return NULL;
    }
    if (f->type == PARITY) {
        memcpy(r->parity, f->data, PART);
        r->par = 1;
    } else {
        size_t part = f->type == DATA1;
        memcpy(r->data + part * PART, f->data, PART);
        r->got |= 1u << part;
    }
    if (r->par && (r->got == 1 || r->got == 2)) {
        size_t have = r->got == 2 ? PART : 0;
        size_t need = PART - have;
        for (size_t i = 0; i < PART; i++) {
            r->data[need + i] = r->parity[i] ^ r->data[have + i];
        }
        r->got = 3;
    }
    if (r->got == all_bits(r->len)) {
        if (!text_valid(r->data, r->len)) {
            return NULL;
        }
        r->done = f->seq;
        r->ok = 1;
        r->next = f->seq + 1;
        c->recv++;
        arm_ack(r, f->seq, r->got | ACK_COMPLETE, slot);
        return r;
    }
    arm_ack(r, f->seq, r->got, slot);
    return NULL;
}

static int
complementary_repair(const struct message *m, unsigned want)
{
    if (want != 3 || !m->n) {
        return 0;
    }
    for (size_t i = 0; i < m->n; i++) {
        unsigned state = m->to[i].bits;
        if ((state & ACK_COMPLETE) && (state & 3) == 3) {
            continue;
        }
        unsigned have = state & 3;
        if (have != 1 && have != 2) {
            return 0;
        }
    }
    return 1;
}

int
make_frame(struct chat *c, uint64_t slot, struct frame *f, int ack_now)
{
    struct remote *ack = NULL;
    for (size_t i = 0; i < PEER_MAX; i++) {
        struct remote *r = &c->peers[i];
        if (r->ack && (!ack || r->ack_at < ack->ack_at)) {
            ack = r;
        }
    }
    if (ack_now && c->ack_src) {
        for (size_t i = 0; i < PEER_MAX; i++) {
            struct remote *r = &c->peers[i];
            if (r->id == c->ack_src) {
                ack = r->ack && r->ack_seq == c->ack_event ? r : NULL;
                break;
            }
        }
    }
    struct message *m = pending(c);
    unsigned want = m ? missing(m) : 0;
    int data = !ack_now && m && m->live && slot >= m->retry && want;
    int repair = data && !m->sent && complementary_repair(m, want);
    int hello = !ack_now && slot >= c->hello;
    if ((ack_now && !ack) || (!ack_now && !data && !hello && !ack)) {
        return 0;
    }
    uint64_t seed = mix(slot);
    if (ack_now) {
        seed ^= mix(ack->id) ^ mix(ack->ack_seq);
    }
    uint64_t score = mix(c->id ^ seed);
    unsigned rank = 0;
    for (size_t i = 0; i < PEER_MAX; i++) {
        struct remote *r = &c->peers[i];
        if (!r->live || (ack_now ? r->id == ack->id : !r->tx_pending)) {
            continue;
        }
        rank += mix(r->id ^ seed) < score;
    }
    if (rank > 1) {
        return 0;
    }
    memset(f, 0, sizeof *f);
    f->src = c->id;
    if (data) {
        unsigned unsent = want & ~m->sent;
        f->seq = m->seq;
        f->len = m->len;
        f->type = repair ? PARITY : unsent & 1 ? DATA0 : unsent & 2 ? DATA1 : PARITY;
        if (f->type == PARITY) {
            for (size_t i = 0; i < PART; i++) {
                f->data[i] = m->data[i] ^ m->data[PART + i];
            }
        } else {
            memcpy(f->data, m->data + (f->type == DATA1 ? PART : 0), PART);
        }
    } else if (ack_now) {
        f->type = ACK;
    } else if (hello) {
        f->type = HELLO;
    } else {
        f->type = ACK;
    }
    int more = m && m->live && want;
    for (size_t i = 0; i < PEER_MAX; i++) {
        more |= c->peers[i].ack && &c->peers[i] != ack;
    }
    if (more) {
        f->flags |= TX_PENDING;
    }
    if (ack) {
        f->flags |= PIGGY;
        f->target = ack->id;
        f->ack_seq = ack->ack_seq;
        f->bits = ack->ack_bits;
    }
    return rank ? 2 : 1;
}

void
sent_frame(struct chat *c, const struct frame *f, uint64_t slot, int complete)
{
    c->sent++;
    c->hello = slot + (live_peers(c) ? HELLO_SLOTS : DISCOVER_SLOTS);
    if (complete && (f->flags & PIGGY)) {
        for (size_t i = 0; i < PEER_MAX; i++) {
            if (c->peers[i].id == f->target) {
                c->peers[i].ack = 0;
            }
        }
    }
    if (f->type != ACK && f->type != HELLO) {
        struct message *m = pending(c);
        unsigned want = missing(m);
        int repair = f->type == PARITY && !m->sent && complementary_repair(m, want);
        m->sent |= 1u << (f->type - DATA0);
        m->retry = slot + 1;
        if (repair) {
            m->sent |= want;
        }
        if ((m->sent & want) == want && (want != 3 || (m->sent & 4))) {
            m->sent = 0;
            int feedback = 0;
            for (size_t i = 0; i < m->n; i++) {
                feedback |= m->to[i].bits & all_bits(m->len);
            }
            if (m->round < (feedback ? 4 : 6)) {
                m->round++;
            }
            m->retry = slot + ((feedback ? 1u : 2u) << m->round);
        }
    }
}
