# Redis Makefile
# Copyright (C) 2009 Salvatore Sanfilippo <antirez at gmail dot com>
# This file is released under the BSD license, see the COPYING file

uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')
OPTIMIZATION ?= -O2
ifeq ($(uname_S),SunOS)
	CFLAGS ?= -std=c99 -pedantic $(OPTIMIZATION) -Wall -W -D__EXTENSIONS__ -D_XPG6
	CCLINK ?= -ldl -lnsl -lsocket -lm -lpthread
else
	CFLAGS ?= -std=c99 -pedantic $(OPTIMIZATION) -Wall -W $(ARCH) $(PROF)
	CCLINK ?= -lm -pthread
endif
CCOPT = $(CFLAGS) $(CCLINK) $(ARCH) $(PROF)
DEBUG ?= -g -rdynamic -ggdb

all: ae.o zmalloc.o
	$(MAKE) -C test

ae.o: ae.c ae.h zmalloc.h config.h ae_select.c
ae_select.o: ae_select.c
zmalloc.o: zmalloc.c config.h

.c.o:
	$(CC) -c $(CFLAGS) $(DEBUG) $(COMPILE_TIME) $<

clean:
	rm -rf *.o *.gcda *.gcno *.gcov
	$(MAKE) -C test clean

dep:
	$(CC) -MM *.c
