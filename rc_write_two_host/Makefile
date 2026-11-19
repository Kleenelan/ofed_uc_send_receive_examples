CC      = gcc
CFLAGS  = -Wall -g 
LDLIBS  = -libverbs

TARGETS = RC_write RC_be_writed

all: $(TARGETS)

RC_write: RC_write.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

RC_be_writed: RC_be_writed.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
