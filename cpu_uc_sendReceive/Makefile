CC=gcc
CFLAGS=-Wall -g
LDFLAGS=-libverbs

all: uc_send_recv

uc_send_recv: uc_send_recv.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f uc_send_recv

.PHONY: all clean
