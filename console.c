#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void
con_wake(void)
{
    (void)!write(cc.con.wake[1], "x", 1);
}

void __attribute__((format(printf, 1, 2)))
con_post(const char *fmt, ...)
{
    va_list ap;
    pthread_mutex_lock(&cc.con.mx);
    if (cc.con.count == OUT_MAX) {
        cc.con.head = (cc.con.head + 1) % OUT_MAX;
        cc.con.count--;
    }
    char *slot = cc.con.out[(cc.con.head + cc.con.count) % OUT_MAX];
    va_start(ap, fmt);
    vsnprintf(slot, OUT_TEXT, fmt, ap);
    va_end(ap);
    cc.con.count++;
    pthread_mutex_unlock(&cc.con.mx);
    con_wake();
}

static void
con_line(char out[OUT_TEXT], const char *s, size_t n)
{
    memcpy(out, "me> ", 4);
    memcpy(out + 4, s, n);
    out[4 + n] = 0;
}

static unsigned
prompt_rows(const char *edit, size_t nedit)
{
    unsigned col = 4;
    unsigned width = cc.con.columns ? cc.con.columns : 80;
    for (size_t i = 0; i < nedit; i++) {
        if (((uint8_t)edit[i] & 0xC0) != 0x80) {
            col++;
        }
    }
    return (col + width - 1) / width;
}

static void
clear_prompt(void)
{
    if (!cc.con.raw) {
        return;
    }
    fputs("\r\033[K", stdout);
    for (unsigned i = 1; i < cc.con.rows; i++) {
        fputs("\033[A\r\033[K", stdout);
    }
    cc.con.rows = 0;
}

void
con_show(void)
{
    char buf[OUT_MAX][OUT_TEXT];
    char edit[LINE_BYTES + 1];
    pthread_mutex_lock(&cc.con.mx);
    size_t n = cc.con.count;
    for (size_t i = 0; i < n; i++) {
        strcpy(buf[i], cc.con.out[(cc.con.head + i) % OUT_MAX]);
    }
    size_t nedit = cc.con.nedit;
    memcpy(edit, cc.con.edit, nedit);
    edit[nedit] = 0;
    cc.con.head = cc.con.count = 0;
    pthread_mutex_unlock(&cc.con.mx);
    struct winsize ws;
    if (!ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) && ws.ws_col) {
        cc.con.columns = ws.ws_col;
    }
    clear_prompt();
    for (size_t i = 0; i < n; i++) {
        fputs(buf[i], stdout);
        putchar('\n');
    }
    if (cc.con.raw && !cc.con.done) {
        char prompt[OUT_TEXT];
        con_line(prompt, edit, nedit);
        fputs(prompt, stdout);
        cc.con.rows = prompt_rows(edit, nedit);
    }
    fflush(stdout);
}

static void
restore_terminal(void)
{
    if (!cc.con.raw) {
        return;
    }
    clear_prompt();
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSANOW, &cc.con.sv);
    cc.con.raw = 0;
}

static void
submit(struct chat *c)
{
    char edit[LINE_BYTES];
    pthread_mutex_lock(&cc.con.mx);
    size_t len = cc.con.nedit;
    int rejected = cc.con.nover;
    memcpy(edit, cc.con.edit, len);
    cc.con.nedit = cc.con.nover = 0;
    pthread_mutex_unlock(&cc.con.mx);
    if (rejected) {
        con_post("line rejected: more than %d bytes", LINE_BYTES);
    } else if (len && enqueue(c, edit, len)) {
        char record[OUT_TEXT];
        con_line(record, edit, len);
        con_post("%s", record);
    } else if (len) {
        con_post("line rejected: queue full");
    }
}

static void
con_key(struct chat *c, unsigned char ch)
{
    if (cc.con.raw && ch == 20) {
        cc.con.req = 1;
        return;
    }
    if (cc.con.raw && ch == 4) {
        cc.stop = -1;
    } else if (ch == '\n' || ch == '\r') {
        submit(c);
    } else if (cc.con.raw && (ch == 8 || ch == 127)) {
        pthread_mutex_lock(&cc.con.mx);
        if (cc.con.nover) {
            cc.con.nover--;
        } else if (cc.con.nedit) {
            do {
                cc.con.nedit--;
            } while (cc.con.nedit &&
                     ((uint8_t)cc.con.edit[cc.con.nedit] & 0xC0) == 0x80);
        }
        pthread_mutex_unlock(&cc.con.mx);
    } else if (ch >= 32 && ch != 127) {
        pthread_mutex_lock(&cc.con.mx);
        if (cc.con.nedit == LINE_BYTES) {
            cc.con.nover++;
        } else {
            cc.con.edit[cc.con.nedit++] = ch;
        }
        pthread_mutex_unlock(&cc.con.mx);
    }
}

void
con_input(struct chat *c)
{
    if (cc.con.eof) {
        return;
    }
    struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
    int ready = poll(&pfd, 1, 0);
    if (ready < 0) {
        if (errno != EINTR) {
            perror("poll");
            cc.stop = -2;
        }
        return;
    }
    if (!ready || !pfd.revents) {
        return;
    }
    unsigned char buf[256];
    ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
    if (n < 0) {
        if (errno != EINTR && errno != EAGAIN) {
            perror("read");
            cc.stop = -2;
        }
        return;
    }
    if (!n) {
        submit(c);
        cc.con.eof = 1;
    } else {
        for (ssize_t i = 0; i < n && !cc.stop && !cc.con.eof; i++) {
            con_key(c, buf[i]);
        }
    }
    con_wake();
}

static void *
console(void *arg)
{
    (void)arg;
    for (;;) {
        con_show();
        if (cc.con.done) {
            break;
        }
        struct pollfd pfd = { cc.con.wake[0], POLLIN, 0 };
        if (poll(&pfd, 1, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            cc.stop = -2;
            break;
        }
        if (pfd.revents) {
            char drained[64];
            while (read(cc.con.wake[0], drained, sizeof drained) > 0) {
            }
        }
    }
    return NULL;
}

void
con_open(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct winsize ws;
    cc.con.columns = !ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) && ws.ws_col ? ws.ws_col : 80;
    if (pipe(cc.con.wake) || fcntl(cc.con.wake[0], F_SETFL, O_NONBLOCK) || fcntl(cc.con.wake[1], F_SETFL, O_NONBLOCK)) {
        die("console pipe", errno);
    }
    atexit(restore_terminal);
    if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) && !tcgetattr(STDIN_FILENO, &cc.con.sv)) {
        struct termios t = cc.con.sv;
        t.c_lflag &= ~(ICANON | ECHO);
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        cc.con.raw = !tcsetattr(STDIN_FILENO, TCSANOW, &t);
    }
    int err = pthread_create(&cc.con.tid, NULL, console, NULL);
    if (err) {
        die("pthread_create", err);
    }
}

void
con_close(void)
{
    cc.con.done = 1;
    con_wake();
    pthread_join(cc.con.tid, NULL);
}
