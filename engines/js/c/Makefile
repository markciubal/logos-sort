CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c99
LDFLAGS ?=
LDLIBS  ?= -lm

PREFIX  ?= /usr/local
LIBDIR  ?= $(PREFIX)/lib
INCDIR  ?= $(PREFIX)/include

LIB     = liblogos_sort.a
SO      = liblogos_sort.so
OBJS    = sort.o

.PHONY: all clean test install

all: $(LIB) $(SO) test

$(LIB): $(OBJS)
	ar rcs $@ $^

$(SO): sort.c sort.h
	$(CC) $(CFLAGS) -fPIC -shared -o $@ sort.c $(LDLIBS)

sort.o: sort.c sort.h
	$(CC) $(CFLAGS) -c sort.c -o sort.o

test: test.c $(LIB) sort.h
	$(CC) $(CFLAGS) -o $@ test.c $(LIB) $(LDLIBS)

clean:
	rm -f $(LIB) $(SO) $(OBJS) test

install: $(LIB) $(SO) sort.h
	install -d $(LIBDIR) $(INCDIR)
	install -m 644 $(LIB) $(LIBDIR)/
	install -m 755 $(SO) $(LIBDIR)/
	install -m 644 sort.h $(INCDIR)/logos_sort.h
