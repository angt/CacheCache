#include "common.h"

#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

static void
stats(const struct chat *c)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    con_post("frames"
             " attempted"   "=%"PRIu64
             " complete"    "=%"PRIu64
             " rx"          "=%"PRIu64
             " sync"        "=%"PRIu64
             " rejected"    "=%"PRIu64
             " late"        "=%"PRIu64
             " aborted"     "=%"PRIu64
             " lines"
             " received"    "=%"PRIu64
             " confirmed"   "=%"PRIu64
             " unconfirmed" "=%"PRIu64
             " missed"      "=%"PRIu64
             " queued"      "=%zu"
             " peers"       "=%zu"
             " cpu_us"      "=%ld"
             " maxrss"      "=%ld",
             c->sent, cc.cnt.tx_ok, cc.cnt.rx, cc.cnt.sync,
                      cc.cnt.rx_bad, cc.cnt.late, cc.cnt.tx_bad,
             c->recv, c->conf, c->fail, c->miss, c->count,
             live_peers(c), ru.ru_utime.tv_sec * 1000000L + ru.ru_utime.tv_usec
                          + ru.ru_stime.tv_sec * 1000000L + ru.ru_stime.tv_usec,
             ru.ru_maxrss);
}

static void
show_received(struct chat *c, const struct frame *f)
{
    pthread_mutex_lock(&c->mx);
    c->ack_src = f->src;
    c->ack_event = f->seq;
    struct remote *r = receive(c, f, now() / SLOT_NS);
    if (r) {
        con_post("%08" PRIx32 "> %.*s", r->id, r->len, r->data);
    }
    pthread_mutex_unlock(&c->mx);
}

static void
on_signal(int sig)
{
    if (sig == SIGINT || sig == SIGTERM || sig == SIGHUP) {
        cc.stop = sig;
    } else {
        cc.con.req = 1;
    }
}

static void
install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
#ifdef SIGINFO
    sigaction(SIGINFO, &sa, NULL);
#else
    sigaction(SIGUSR1, &sa, NULL);
#endif
}

int
main(void)
{
    cc.dbg = !!getenv("DEBUG");
    struct chat *c = &cc.chat;
    c->id = mix(now() ^ ((uint64_t)getpid() << 32) ^ (uintptr_t)c);
    if (!c->id) {
        c->id = 1;
    }
    install_signals();
    con_open(c);
    modem_open();
    uint64_t first = now() / SLOT_NS + 1;
    uint64_t last = UINT64_MAX;
    uint64_t quick = UINT64_MAX;
    uint64_t received_at = UINT64_MAX;
    uint64_t eof_deadline = UINT64_MAX;
    while (!cc.stop) {
        uint64_t slot = now() / SLOT_NS;
        struct frame f;
        int transmit = 0;
        int response = 0;
        pthread_mutex_lock(&c->mx);
        tick(c, slot);
        int acks = 0;
        for (size_t i = 0; i < PEER_MAX; i++) {
            acks |= c->peers[i].ack;
        }
        if (cc.con.eof && !c->count && eof_deadline == UINT64_MAX) {
            eof_deadline = slot + EOF_ACK_SLOTS;
        }
        int done = cc.con.eof && !c->count && (!acks || slot >= eof_deadline);
        if (atomic_exchange(&cc.con.req, 0)) {
            stats(c);
        }
        int fast = quick == slot;
        quick = UINT64_MAX;
        int fresh_ack = 0;
        if (fast && c->ack_src) {
            for (size_t i = 0; i < PEER_MAX; i++) {
                struct remote *r = &c->peers[i];
                fresh_ack |= r->id == c->ack_src && r->ack && r->ack_seq == c->ack_event;
            }
        }
        if (!done && slot >= first && slot != last) {
            if (cc.con.eof && !c->count && acks) {
                c->ack_src = 0;
                transmit = make_frame(c, slot, &f, 1);
                response = fast && transmit;
            } else {
                if (fast) {
                    transmit = make_frame(c, slot, &f, 1);
                    response = transmit != 0;
                }
                if (!transmit && !fresh_ack) {
                    transmit = make_frame(c, slot, &f, 0);
                }
            }
        }
        pthread_mutex_unlock(&c->mx);
        if (done) {
            break;
        }
        if (transmit == 2 && !cc.stop) {
            uint64_t quiet = (response ? received_at : slot * SLOT_NS) + CONTEND_NS;
            uint64_t deadline = (slot + 1) * SLOT_NS;
            if (now() >= quiet) {
                transmit = 0;
            } else if (hear(&f, quiet, deadline)) {
                received_at = now();
                show_received(c, &f);
                quick = now() / SLOT_NS;
                continue;
            }
        }
        if (transmit) {
            struct frame incoming;
            int sent = emit(&f, &incoming, slot);
            if (sent == 2) {
                received_at = now();
                show_received(c, &incoming);
                quick = now() / SLOT_NS;
                continue;
            }
            if (sent >= 0) {
                last = slot;
                pthread_mutex_lock(&c->mx);
                sent_frame(c, &f, slot, sent);
                pthread_mutex_unlock(&c->mx);
                continue;
            }
        }
        uint64_t deadline = (slot + 1) * SLOT_NS;
        if (hear(&f, deadline, deadline)) {
            received_at = now();
            show_received(c, &f);
            quick = now() / SLOT_NS;
        }
    }
    con_close();
    if (c->count) {
        con_post("%zu queued line(s) still unconfirmed", c->count);
    }
    con_show();
    modem_close();
    return cc.stop > 0 ? 128 + cc.stop : cc.stop == -2;
}
