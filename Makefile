CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -O2
OBJS = main.o race.o io_util.o

all: autogonki

autogonki: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

.c.o:
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f $(OBJS) autogonki race.log

.PHONY: all clean test
test: autogonki
	./tests/run.sh
