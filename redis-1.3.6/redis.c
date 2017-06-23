/*
 * Copyright (c) 2009-2010, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#define REDIS_VERSION "1.3.6"

#include "fmacros.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define __USE_POSIX199309
#define __USE_UNIX98
#include <signal.h>

#ifdef HAVE_BACKTRACE
#include <execinfo.h>
#include <ucontext.h>
#endif /* HAVE_BACKTRACE */

#include <sys/wait.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>

#if defined(__sun)
#include "solarisfixes.h"
#endif

#include "redis.h"
#include "ae.h"         /* Event driven programming library */
#include "sds.h"        /* Dynamic safe strings */
#include "anet.h"       /* Networking the easy way */
#include "dict.h"       /* Hash tables */
#include "adlist.h"     /* Linked lists */
#include "zmalloc.h"    /* total memory usage aware version of malloc/free */
// #include "lzf.h"        /* LZF compression library */
// #include "pqsort.h"     /* Partial qsort for SORT+LIMIT */
// #include "zipmap.h"

/* Error codes */
#define REDIS_OK    0
#define REDIS_ERR  -1

/* Static server configuration */
#define REDIS_SERVERPORT        6379    /* TCP port */
#define REDIS_MAXIDLETIME       (60*5)  /* default client timeout */
#define REDIS_IOBUF_LEN         1024
#define REDIS_LOADBUF_LEN       1024
#define REDIS_STATIC_ARGS       4
#define REDIS_DEFAULT_DBNUM     16
#define REDIS_CONFIGLINE_MAX    1024
#define REDIS_OBJFREELIST_MAX   1000000 /* Max number of objects to cache */
#define REDIS_MAX_SYNC_TIME     60      /* Slave can't take more to sync */
#define REDIS_EXPIRELOOKUPS_PER_CRON    100 /* try to expire 100 keys/second */
#define REDIS_MAX_WRITE_PER_EVENT (1024*64)
#define REDIS_REQUEST_MAX_SIZE (1024*1024*256) /* max bytes in inline command */

/* Log levels */
#define REDIS_DEBUG     0
#define REDIS_VERBOSE   1
#define REDIS_NOTICE    2
#define REDIS_WARNING   3

/* Anti-warning macro... */
#define REDIS_NOTUSED(V) ((void) V)

/*================================= Data types ============================== */

/* With multiplexing we need to take per-clinet state.
 * Clients are taken in a liked list. */
typedef struct redisClient {
    int fd;
    sds querybuf;
    time_t lastinteraction; /* time of the last interaction, used for timeout */
} redisClient;

/* Global server state structure */
struct redisServer {
    int port;
    int fd;
    list *clients;
    char neterr[ANET_ERR_LEN];
    aeEventLoop *el;
    int cronloops;              /* number of times the cron function run */
    /* Fields used only for stats */
    time_t stat_starttime;         /* server start time */
    long long stat_numcommands;    /* number of processed commands */
    long long stat_numconnections; /* number of connections received */
    /* Configuration */
    int verbosity;
    int maxidletime;
    int dbnum;
    int daemonize;
    char *pidfile;
    char *logfile;
    char *bindaddr;
    unsigned int maxclients;
    time_t unixtime;    /* Unix time sampled every second. */
};

/*================================ Prototypes =============================== */

static void freeClient(redisClient *c);
static void acceptHandler(aeEventLoop *el, int fd, void *privdata, int mask);

/*================================= Globals ================================= */

/* Global vars */
static struct redisServer server; /* server global state */

/*============================ Utility functions ============================ */

static void redisLog(int level, const char *fmt, ...) {
    va_list ap;
    FILE *fp;

    fp = (server.logfile == NULL) ? stdout : fopen(server.logfile, "a");
    if (!fp) return;

    va_start(ap, fmt);
    if (level >= server.verbosity) {
        char *c = ".-*#";
        char buf[64];
        time_t now;

        now = time(NULL);
        strftime(buf, 64, "%d %b %H:%M:%S", localtime(&now));
        fprintf(fp, "[%d] %s %c ", (int) getpid(), buf, c[level]);
        vfprintf(fp, fmt, ap);
        fprintf(fp, "\n");
        fflush(fp);
    }
    va_end(ap);

    if (server.logfile) fclose(fp);
}

/* ========================= Random utility functions ======================= */

/* Redis generally does not try to recover from out of memory conditions
 * when allocating objects or strings, it is not clear if it will be possible
 * to report this condition to the client since the networking layer itself
 * is based on heap allocation for send buffers, so we simply abort.
 * At least the code will be simpler to read... */
static void oom(const char *msg) {
    redisLog(REDIS_WARNING, "%s: Out of memory\n", msg);
    sleep(1);
    abort();
}

static int serverCron(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    int j, loops = server.cronloops++;
    REDIS_NOTUSED(eventLoop);
    REDIS_NOTUSED(id);
    REDIS_NOTUSED(clientData);

    /* We take a cached value of the unix time in the global state because
     * with virtual memory and aging there is to store the current time
     * in objects at every object access, and accuracy is not needed.
     * To access a global var is faster than calling time(NULL) */
    server.unixtime = time(NULL);

    return 1000;
}

static void initServerConfig() {
    server.dbnum = REDIS_DEFAULT_DBNUM;
    server.port = REDIS_SERVERPORT;
    server.verbosity = REDIS_VERBOSE;
    server.maxidletime = REDIS_MAXIDLETIME;
    // server.saveparams = NULL;
    server.logfile = NULL;  /* NULL = log on standard output */
    server.bindaddr = NULL;
    server.daemonize = 0;
}

static void initServer() {
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    server.clients = listCreate();
    server.el = aeCreateEventLoop();
    server.fd = anetTcpServer(server.neterr, server.port, server.bindaddr);
    if (server.fd == -1) {
        redisLog(REDIS_WARNING, "Opening TCP port: %s", server.neterr);
        exit(1);
    }

    aeCreateTimeEvent(server.el, 1, serverCron, NULL, NULL);
    if (aeCreateFileEvent(server.el, server.fd, AE_READABLE,
        acceptHandler, NULL) == AE_ERR) oom("creating file event");
}

static void loadServerConfig(char *filename) {
    FILE *fp;
    char buf[REDIS_CONFIGLINE_MAX+1], *err = NULL;
    int linenum = 0;
    sds line = NULL;

    if (filename[0] == '-' && filename[1] == '\0')
        fp = stdin;
    else {
        if ((fp = fopen(filename, "r")) == NULL) {
            redisLog(REDIS_WARNING, "Fatal error, can't open config file");
            exit(1);
        }
    }

    while (fgets(buf, REDIS_CONFIGLINE_MAX+1, fp) != NULL) {
        sds *argv;
        int argc, j;

        linenum++;
        line = sdsnew(buf);
        line = sdstrim(line, " \t\r\n");

        /* Skip comments and blank lines */
        if (line[0] == '#' || line[0] == '\0') {
            sdsfree(line);
            continue;
        }

        /* Split into arguments */
        argv = sdssplitlen(line, sdslen(line), " ", 1, &argc);
        sdstolower(argv[0]);

        /* Execute config directives */
        if (!strcasecmp(argv[0], "timeout") && argc == 2) {
            server.maxidletime = atoi(argv[1]);
            if (server.maxidletime < 0) {
                err = "Invalid timeout value"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0], "port") && argc == 2) {
            server.port = atoi(argv[1]);
            if (server.port < 1 || server.port > 65535) {
                err = "Invalid port"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0], "bind") && argc == 2) {
            server.bindaddr = zstrdup(argv[1]);
        } else {
            err = "Bad directive or wrong number of arguments"; goto loaderr;
        }
        for (j = 0; j < argc; j++)
            sdsfree(argv[j]);
        zfree(argv);
        sdsfree(line);
    }
    if (fp != stdin) fclose(fp);
    return;

loaderr:
    fprintf(stderr, "\n*** FATAL CONFIG FILE ERROR ***\n");
    fprintf(stderr, "Reading the configuration file, at line %d\n", linenum);
    fprintf(stderr, ">>> '%s'\n", line);
    fprintf(stderr, "%s\n", err);
    exit(1);
}

static void freeClient(redisClient *c) {
    listNode *ln;

    /* Note that if the client we are freeing is blocked into a blocking
     * call, we have to set querybuf to NULL *before* to call
     * unblockClientWaitingData() to avoid processInputBuffer() will get
     * called. Also it is important to remove the file events after
     * this, because this call adds the READABLE event. */
    sdsfree(c->querybuf);
    c->querybuf = NULL;

    aeDeleteFileEvent(server.el, c->fd, AE_READABLE);
    aeDeleteFileEvent(server.el, c->fd, AE_WRITABLE);
    close(c->fd);
    /* Remove from the list of clients */
    ln = listSearchKey(server.clients, c);
    // redisAssert(ln != NULL);
    listDelNode(server.clients, ln);
    zfree(c);
}

/* If this function gets called we already read a whole
 * command, argments are in the client argv/argc fields.
 * processCommand() execute the command or prepare the
 * server for a bulk read from the client.
 *
 * If 1 is returned the client is still alive and valid and
 * and other operations can be performed by the caller. Otherwise
 * if 0 is returned the client was destroied (i.e. after QUIT). */
static int processCommand(redisClient *c) {
    /* The QUIT command is handled as a special case. Normal command
     * procs are unable to close the client connection safely */
    if (!strcasecmp(c->argv[0]->ptr, "quit")) {
        freeClient(c);
        return 0;
    }

    /* Now lookup the command and check ASAP about trivial error conditions
     * such wrong arity, bad command name and so forth. */
    addReplySds(c, sdscatprintf(sdsempty(), "-ERR unknown command '%s'\r\n",
        (char *) c->argv[0]->ptr));
    return 1;
}

static void processInputBuffer(redisClient *c) {
again:
    /* Before to process the input buffer, make sure the client is not
     * waitig for a blocking operation such as BLPOP. Note that the first
     * iteration the client is never blocked, otherwise the processInputBuffer
     * would not be called at all, but after the execution of the first commands
     * in the input buffer the client may be blocked, and the "goto again"
     * will try to reiterate. The following line will make it return asap. */
    if (c->flags & REDIS_BLOCKED || c->flags & REDIS_IO_WAIT) return;
    if (c->bulklen == -1) {
        /* Read the first line of the query */
        char *p = strchr(c->querybuf, '\n');
        size_t querylen;

        if (p) {
            sds query, *argv;
            int argc, j;

            query = c->querybuf;
            c->querybuf = sdsempty();
            querylen = 1 + (p - query);
            if (sdslen(query) > querylen) {
                /* leave data after the first line of the query in the buffer */
                c->querybuf = sdscatlen(c->querybuf, query + querylen, sdslen(query) - querylen);
            }
            *p = '\0';  /* remove "\n" */
            if (*(p - 1) == '\r') *(p - 1) = '\0'; /* and "\r" if any */
            sdsupdatelen(query);

            /* Now we can split the query in arguments */
            argv = sdssplitlen(query, sdslen(query), " ", 1, &argc);
            sdsfree(query);

            if (c->argv) zfree(c->argv);
            c->argv = zmalloc(sizeof(robj *) * argc);

            for (j = 0; j < argc; j++) {
                if (sdslen(argv[j])) {
                    c->argv[c->argc] = createObject(REDIS_STRING, argv[j]);
                    c->argc++;
                } else {
                    sdsfree(argv[j]);
                }
            }
            zfree(argv);
            if (c->argc) {
                /* Execute the command. If the client is still valid
                 * after processCommand() return and there is something
                 * on the query buffer try to process the next command. */
                if (processCommand(c) && sdslen(c->querybuf)) goto again;
            } else {
                /* Nothing to process, argc == 0. Just process the query
                 * buffer if it's not empty or return to the caller */
                if (sdslen(c->querybuf)) goto again;
            }
            return;
        } else if (sdslen(c->querybuf) >= REDIS_REQUEST_MAX_SIZE) {
            redisLog(REDIS_VERBOSE, "Client protocol error");
            freeClient(c);
            return;
        }
    } else {

    }
}

static void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    redisClient *c = (redisClient *) privdata;
    char buf[REDIS_IOBUF_LEN];
    int nread;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);

    nread = read(fd, buf, REDIS_IOBUF_LEN);
    if (nread == -1) {
        if (errno == EAGAIN) {
            nread = 0;
        } else {
            redisLog(REDIS_VERBOSE, "Reading from client: %s", strerror(errno));
            freeClient(c);
            return;
        }
    } else if (nread == 0) {
        redisLog(REDIS_VERBOSE, "Client closed connection");
        freeClient(c);
        return;
    }
    if (nread) {
        c->querybuf = sdscatlen(c->querybuf, buf, nread);
        c->lastinteraction = time(NULL);
    } else {
        return;
    }
    if (!(c->flags & REDIS_BLOCKED))
        processInputBuffer(c);
}

static redisClient *createClient(int fd) {
    redisClient *c = zmalloc(sizeof(*c));

    anetNonBlock(NULL, fd);
    anetTcpNoDelay(NULL, fd);
    if (!c) return NULL;
    c->fd = fd;
    c->querybuf = sdsempty();
    if (aeCreateFileEvent(server.el, c->fd, AE_READABLE,
        readQueryFromClient, c) == AE_ERR) {
        freeClient(c);
        return NULL;
    }
    listAddNodeTail(server.clients, c);
    return c;
}

static void acceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd;
    char cip[128];
    redisClient *c;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);
    REDIS_NOTUSED(privdata);

    cfd = anetAccept(server.neterr, fd, cip, &cport);
    if (cfd == AE_ERR) {
        redisLog(REDIS_VERBOSE, "Accepting client connection: %s", server.neterr);
        return;
    }
    redisLog(REDIS_VERBOSE, "Accepted %s:%d", cip, cport);
    if ((c = createClient(cfd)) == NULL) {
        redisLog(REDIS_WARNING, "Error allocating resources for the client");
        close(cfd); /* May be already closed, just ignore errors */
        return;
    }
    /* If maxclient directive is set and this is one client more... close the
     * connection. Note that we create the client instead to check before
     * for this condition, since now the socket is already set in nonblocking
     * mode and we can send an error for free using the Kernel I/O */
    if (server.maxclients && listLength(server.clients) > server.maxclients) {
        char *err = "-ERR max number of clients reached\r\n";

        /* That's a best effort error message, don't check write errors */
        if (write(c->fd, err, strlen(err)) == -1) {
            /* Nothing to do, Just to avoid the warning... */
        }
        freeClient(c);
        return;
    }
    server.stat_numconnections++;
}

/* =================================== Main! ================================ */

#ifdef __linux__
int linuxOvercommitMemoryValue(void) {
    FILE *fp = fopen("/proc/sys/vm/overcommit_memory", "r");
    char buf[64];

    if (!fp) return -1;
    if (fgets(buf, 64, fp) == NULL) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    return atoi(buf);
}

void linuxOvercommitMemoryWarning(void) {
    if (linuxOvercommitMemoryValue() == 0) {
        redisLog(REDIS_WARNING, "WARNING overcommit_memory is set to 0! Background save may fail under low condition memory. To fix this issue add 'vm.overcommit_memory = 1' to /etc/sysctl.conf and then reboot or run the command 'sysctl vm.overcommit_memory=1' for this to take effect.");
    }
}
#endif /* __linux__ */

static void daemonize(void) {
    int fd;
    FILE *fp;

    if (fork() != 0) exit(0); /* parent exits */
    setsid(); /* create a new session */

    /* Every output goes to /dev/null. If Redis is daemonized but
     * the 'logfile' is set to 'stdout' in the configuration file
     * it will not log at all. */
    if ((fd = open("/dev/null", O_RDWR)) != -1) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }
    /* Try to write the pid file */
    fp = fopen(server.pidfile, "w");
    if (fp) {
        fprintf(fp, "%d\n", getpid());
        fclose(fp);
    }
}

int main(int argc, char **argv) {
    // time_t start;

    initServerConfig();
    if (argc == 2) {
        // resetServerSaveParams();
        loadServerConfig(argv[1]);
    } else if (argc > 2) {
        fprintf(stderr, "Usage: ./redis-server [/path/to/redis.conf]\n");
        exit(1);
    } else {
        redisLog(REDIS_WARNING, "Warning: no config file specified, using the default config. In order to specify a config file use 'redis-server /path/to/redis.conf'");
    }
    if (server.daemonize) daemonize();
    initServer();
    redisLog(REDIS_NOTICE, "server started, Redis version " REDIS_VERSION);
#ifdef __linux__
    linuxOvercommitMemoryWarning();
#endif
    redisLog(REDIS_NOTICE, "The server is now ready to accept connections on port %d", server.port);
    aeMain(server.el);
    aeDeleteEventLoop(server.el);
    return 0;
}
