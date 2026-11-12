CC     ?= gcc
CFLAGS ?= -O1 -fno-if-conversion -fno-if-conversion2 -fno-inline \
          -fno-optimize-sibling-calls -Wall
PROGS  := loop sort recursion dispatch

all: $(PROGS)

%: %.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(PROGS) *.stats
.PHONY: all clean
