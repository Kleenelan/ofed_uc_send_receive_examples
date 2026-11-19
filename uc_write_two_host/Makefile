CC      = gcc
CFLAGS  = -Wall
LDLIBS  = -libverbs

TARGETS = UC_write UC_be_writed

all: $(TARGETS)

UC_write: UC_write.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

UC_be_writed: UC_be_writed.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
