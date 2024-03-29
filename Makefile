# vim:ts=8

CC	?= cc
CFLAGS	?= -O2
CFLAGS	+= -Wall -Wunused -Wmissing-prototypes -Wstrict-prototypes -Wunused

PREFIX	?= /usr/local
BINDIR	?= $(DESTDIR)$(PREFIX)/bin
MANDIR	?= $(DESTDIR)$(PREFIX)/man/man1

INSTALL_PROGRAM ?= install -s
INSTALL_DATA ?= install

X11BASE	?= /usr/X11R6
INCLUDES?= -I$(X11BASE)/include
LDPATH	?= -L$(X11BASE)/lib
LIBS	+= -lX11 -lXrandr -lXext -lXi -lm

PROG	= xdimmer
OBJS	= xdimmer.o

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(OBJS) $(LDPATH) $(LIBS) -o $@

$(OBJS): *.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

README.md: xdimmer.1
	mandoc -T markdown xdimmer.1 > README.md

install: all
	mkdir -p $(BINDIR)
	$(INSTALL_PROGRAM) $(PROG) $(BINDIR)
	mkdir -p $(MANDIR)
	$(INSTALL_DATA) -m 644 xdimmer.1 $(MANDIR)/xdimmer.1

clean:
	rm -f $(PROG) $(OBJS)

.PHONY: all install clean
