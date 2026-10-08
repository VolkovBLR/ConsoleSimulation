#include "io_util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void io_write(int fd, const char *s) {
    size_t n = strlen(s);
    while (n > 0) {
        ssize_t w = write(fd, s, n);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        s += (size_t)w;
        n -= (size_t)w;
    }
}

void io_printf(int fd, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    io_write(fd, buf);
}

void io_sleep_ms(int ms) {
    if (ms <= 0) {
        return;
    }
    usleep((useconds_t)ms * 1000);
}
