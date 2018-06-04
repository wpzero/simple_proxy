CFLAGS=-O2 -std=c99 -Wall -DUSE_SPLICE -g
LDFLAGS=

all:
	gcc $(CFLAGS) -o proxy main.c $(LDFLAGS)

clean:
	rm -f proxy
