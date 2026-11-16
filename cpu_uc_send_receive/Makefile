CC=gcc
CFLAGS=-Wall -g
LDFLAGS=-libverbs

BIN=uc_send uc_receive

all: $(BIN)

uc_send: uc_send.c uc_common.c uc_common.h
	$(CC) $(CFLAGS) -o $@ uc_send.c uc_common.c $(LDFLAGS)

uc_receive: uc_receive.c uc_common.c uc_common.h
	$(CC) $(CFLAGS) -o $@ uc_receive.c uc_common.c $(LDFLAGS)

clean:
	rm -f $(BIN)

.PHONY: all clean
