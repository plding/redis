# Redis Makefile

uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')
OPTIMIZATION?=-O2
ifeq ($(uname_S),SunOS)
  CFLAGS?= -std=c99 -pedantic $(OPTIMIZATION) -Wall -W -D__EXTENSIONS__ -D_XPG6
  CCLINK?= -ldl -lnsl -lsocket -lm -lpthread
else
  CFLAGS?= -std=c99 -pedantic $(OPTIMIZATION) -Wall -W $(ARCH) $(PROF)
  CCLINK?= -lm -pthread
endif
CCOPT= $(CFLAGS) $(CCLINK) $(ARCH) $(PROF)
DEBUG?= -g -rdynamic -ggdb

OBJ = redis.o zmalloc.o sds.o anet.o ae.o adlist.o dict.o
CLIOBJ = redis-cli.o zmalloc.o sds.o anet.o

PROGRAM = redis-server
CLIPROGRAM = redis-cli

all: $(PROGRAM) $(CLIPROGRAM)

$(PROGRAM): $(OBJ)
	$(CC) -o $(PROGRAM) $(CCOPT) $(DEBUG) $(OBJ)

$(CLIPROGRAM): $(CLIOBJ)
	$(CC) -o $(CLIPROGRAM) $(CCOPT) $(DEBUG) $(CLIOBJ)

clean:
	rm -rf $(PROGRAM) $(CLIPROGRAM) *.o
