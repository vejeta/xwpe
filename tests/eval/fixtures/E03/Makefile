CC ?= cc
CFLAGS ?= -Wall -O0

basket: main.c items.c items.h
	$(CC) $(CFLAGS) -o basket main.c items.c

test: basket
	./basket | grep -x 'basket=1399'
	./basket | grep -x 'empty=0'

clean:
	rm -f basket

.PHONY: test clean
