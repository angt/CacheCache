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
    while (write(cc.con.wake[1], "x", 1) < 0 && errno == EINTR);
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

static int
submit(void)
{
    size_t len = cc.con.nedit;
    if (!len) {
        return 1;
    }
    pthread_mutex_lock(&cc.con.mx);
    int full = cc.con.nlines == QUEUE_MAX;
    if (!full) {
        size_t tail = (cc.con.line_head + cc.con.nlines) % QUEUE_MAX;
        struct con_line *line = &cc.con.lines[tail];
        line->len = len;
        line->rejected = !!cc.con.nover;
        memcpy(line->data, cc.con.edit, len);
        cc.con.nlines++;
    }
    pthread_mutex_unlock(&cc.con.mx);
    if (full) {
        if (cc.con.batch) {
            return 0;
        }
        con_post("line rejected: queue full");
    }
    cc.con.nedit = cc.con.nover = 0;
    return 1;
}

static int
con_key(unsigned char ch)
{
    enum { KEY_TEXT, KEY_ESCAPE, KEY_SEQUENCE };
    if (cc.con.raw) {
        if (ch == '\033') {
            cc.con.escape = KEY_ESCAPE;
            return 1;
        }
        if (cc.con.escape == KEY_ESCAPE && (ch == '[' || ch == 'O')) {
            cc.con.escape = KEY_SEQUENCE;
            return 1;
        }
        if (cc.con.escape == KEY_SEQUENCE && ch >= ' ' && ch <= '~') {
            if (ch >= '@') {
                cc.con.escape = KEY_TEXT;
            }
            return 1;
        }
        cc.con.escape = KEY_TEXT;
    }
    if (cc.con.raw && ch == 20) {
        cc.con.req = 1;
    } else if (cc.con.raw && ch == 4) {
        cc.stop = -1;
    } else if (ch == '\n' || ch == '\r') {
        return submit();
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
    return 1;
}

void
con_input(struct chat *c)
{
    for (size_t i = 0; i < QUEUE_MAX; i++) {
        pthread_mutex_lock(&cc.con.mx);
        if (!cc.con.nlines || (cc.con.batch && !cc.con.done && c->count == QUEUE_MAX)) {
            pthread_mutex_unlock(&cc.con.mx);
            return;
        }
        struct con_line line = cc.con.lines[cc.con.line_head];
        cc.con.line_head = (cc.con.line_head + 1) % QUEUE_MAX;
        cc.con.nlines--;
        pthread_mutex_unlock(&cc.con.mx);
        if (line.rejected) {
            cc.con.rejected = 1;
            con_post("line rejected: more than %d bytes", LINE_BYTES);
        } else if (enqueue(c, line.data, line.len)) {
            char record[OUT_TEXT];
            con_line(record, line.data, line.len);
            con_post("%s", record);
        } else {
            cc.con.rejected = 1;
            con_post("line rejected: queue full");
        }
    }
}

static void *
console(void *arg)
{
    (void)arg;
    unsigned char buf[256];
    size_t pos = 0, len = 0;
    int eof = 0;
    for (;;) {
        while (pos < len && !cc.stop && con_key(buf[pos])) {
            pos++;
        }
        if (eof && !cc.con.eof && submit()) {
            pthread_mutex_lock(&cc.con.mx);
            cc.con.eof = !cc.con.nlines;
            pthread_mutex_unlock(&cc.con.mx);
        }
        con_show();
        if (cc.con.done || cc.stop) {
            break;
        }
        struct pollfd pfd[2] = {
            {eof || pos < len ? -1 : STDIN_FILENO, POLLIN, 0},
            {cc.con.wake[0],                       POLLIN, 0},
        };
        if (poll(pfd, 2, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            cc.stop = -2;
            break;
        }
        if (pfd[1].revents) {
            char drained[64];
            while (read(cc.con.wake[0], drained, sizeof drained) > 0) {
            }
        }
        if (!pfd[0].revents) {
            continue;
        }
        ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            perror("read");
            cc.stop = -2;
            break;
        }
        pos = 0;
        len = (size_t)n;
        eof = !n;
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
    cc.con.batch = !isatty(STDIN_FILENO);
    if (!cc.con.batch && isatty(STDOUT_FILENO) && !tcgetattr(STDIN_FILENO, &cc.con.sv)) {
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
