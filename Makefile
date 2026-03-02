CC=gcc
PROG=ctag
CURSES=-lncurses -lmenu -lform
CURSESW=-lncursesw -lmenuw -lformw -D_CURSESW_WIDE
FILES=ctag.c
CFLAGS+=-std=c11 -pedantic -Wall
LDFLAGS+=-L/usr/local/lib

all:
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $(CURSES) $(LIBS) $(FILES) -o $(PROG)
