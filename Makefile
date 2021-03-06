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

OBJ = ae.o redis.o zmalloc.o

PRGNAME = redis-server

all: redis-server

# Deps (use make dep to generate this)
ae.o: ae.c ae.h zmalloc.h config.h ae_epoll.c
ae_epoll.o: ae_epoll.c
ae_select.o: ae_select.c
redis.o: redis.c fmacros.h config.h redis.h ae.h zmalloc.h
zmalloc.o: zmalloc.c config.h

redis-server: $(OBJ)
	$(CC) -o $(PRGNAME) $(CCOPT) $(DEBUG) $(OBJ)
	@echo ""
	@echo "Hint: To run the test-redis.tcl script is a good idea."
	@echo "Launch the redis server with ./redis-server, then in another"
	@echo "terminal window enter this directory and run 'make test'."
	@echo ""

.c.o:
	$(CC) -c $(CFLAGS) $(DEBUG) $(COMPILE_TIME) $<

clean:
	rm -rf $(PRGNAME) *.o *.gcda *.gcno *.gcov

dep:
	$(CC) -MM *.c
