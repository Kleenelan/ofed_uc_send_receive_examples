CC=gcc
CFLAGS=-Wall -g
LDFLAGS=-libverbs

BIN=uc_send uc_receive

all: $(BIN)

uc_send: uc_send.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

uc_receive: uc_receive.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(BIN)

.PHONY: all clean
