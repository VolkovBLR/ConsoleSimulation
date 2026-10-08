#ifndef IO_UTIL_H
#define IO_UTIL_H

/* Вывод через write(2), без stdio-буферов на дескрипторе. */

void io_write(int fd, const char *s);
void io_printf(int fd, const char *fmt, ...);
void io_sleep_ms(int ms);

#endif
