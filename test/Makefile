CC = gcc
CFLAGS = -std=c99 -O2 -Wall -W -I..

PROGS = event-test

all: $(PROGS)

event-test: event-test.o ../ae.o ../zmalloc.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

clean:
	rm -f $(PROGS) *.o event.fifo ../event.fifo
