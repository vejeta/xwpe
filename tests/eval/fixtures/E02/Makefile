CC ?= cc
CFLAGS ?= -Wall -O0

check: check.c stats.c stats.h
	$(CC) $(CFLAGS) -o check check.c stats.c

test: check
	./check

clean:
	rm -f check

.PHONY: test clean
