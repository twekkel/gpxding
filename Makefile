prefix = $(DESTDIR)/usr/local
bindir = ${prefix}/bin
mandir = ${prefix}/share/man

CC       ?= gcc
CFLAGS   += -O2 -std=c11 -Wall -pedantic

INSTALL ?= install -c
STRIP   ?= strip -s

all: gpxding

gpxding: gpxding.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o gpxding gpxding.c

install: all
	$(STRIP) gpxding
	mkdir -p $(bindir)
	$(INSTALL) -m 755 gpxding $(bindir)/gpxding

clean:
	rm -f gpxding
