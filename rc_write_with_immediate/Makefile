CC      = gcc
CFLAGS  = -O2 -Wall
LDLIBS  = -libverbs

TARGETS = rcwrite_a rcwrite_b

all: $(TARGETS)

rcwrite_a: rcwrite_a.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

rcwrite_b: rcwrite_b.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
