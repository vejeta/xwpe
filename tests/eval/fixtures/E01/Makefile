CC ?= cc
CFLAGS ?= -Wall -O0

calc: main.c driver.c front.c parser.c calc.h
	$(CC) $(CFLAGS) -o calc main.c driver.c front.c parser.c

test: calc
	./calc 'a=1;b=2;c=3' | grep -x '3 statements'

clean:
	rm -f calc

.PHONY: test clean
