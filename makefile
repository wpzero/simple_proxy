CC = gcc
CFLAGS = -O2 -std=c99 -Wall -DUSE_SPLICE -g
LDFLAGS =
LIBS = -lpthread
SRCS = main.c sblist.c sblist_delete.c
DEPENDS=$(SRCS:%.c=%.d)
OBJS = $(SRCS:.c=.o)
PROG = proxy

all: $(PROG)

-include $(DEPENDS)

$(PROG): $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) $(LIBS) -o $(PROG)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(DEPENDS):%.d:%.c
	set -e; rm -f $@; \
	$(CC) -MM $(CFLAGS) $< > $@.$$$$; \
	sed 's,\($*\)\.o[:]*,\1.o $@:,g' < $@.$$$$ > $@; \
	rm $@.$$$$

clean:
	-rm -f $(PROG) $(OBJS) $(DEPENDS)
