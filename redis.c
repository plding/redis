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
#include "ae.h"     /* Event driven programming library */
#include "sds.h"    /* Dynamic safe strings */
#include "anet.h"   /* Networking the easy way */
#include "dict.h"   /* Hash tables */
#include "adlist.h" /* Linked lists */
#include "zmalloc.h" /* total memory usage aware version of malloc/free */

/* Error codes */
#define REDIS_OK                0
#define REDIS_ERR               -1

/* Static server configuration */
#define REDIS_SERVERPORT        6379    /* TCP port */
#define REDIS_IOBUF_LEN         1024
#define REDIS_DEFAULT_DBNUM     16
#define REDIS_MAX_WRITE_PER_EVENT (1024*64)
#define REDIS_REQUEST_MAX_SIZE (1024*1024*256) /* max bytes in inline command */

/* Command flags */
#define REDIS_CMD_BULK          1       /* Bulk write command */
#define REDIS_CMD_INLINE        2       /* Inline command */
/* REDIS_CMD_DENYOOM reserves a longer comment: all the commands marked with
   this flags will return an error when the 'maxmemory' option is set in the
   config file and the server is using more than maxmemory bytes of memory.
   In short this commands are denied on low memory conditions. */
#define REDIS_CMD_DENYOOM       4

/* Object types */
#define REDIS_STRING 0
#define REDIS_LIST 1
#define REDIS_SET 2
#define REDIS_ZSET 3
#define REDIS_HASH 4

/* Objects encoding. Some kind of objects like Strings and Hashes can be
 * internally represented in multiple ways. The 'encoding' field of the object
 * is set to one of this fields for this object. */
#define REDIS_ENCODING_RAW 0    /* Raw representation */
#define REDIS_ENCODING_INT 1    /* Encoded as integer */
#define REDIS_ENCODING_ZIPMAP 2 /* Encoded as zipmap */
#define REDIS_ENCODING_HT 3     /* Encoded as an hash table */

/* Client flags */
#define REDIS_SLAVE 1       /* This client is a slave server */
#define REDIS_MASTER 2      /* This client is a master server */
#define REDIS_MONITOR 4     /* This client is a slave monitor, see MONITOR */
#define REDIS_MULTI 8       /* This client is in a MULTI context */
#define REDIS_BLOCKED 16    /* The client is waiting in a blocking operation */
#define REDIS_IO_WAIT 32    /* The client is waiting for Virtual Memory I/O */

/* Log levels */
#define REDIS_DEBUG 0
#define REDIS_VERBOSE 1
#define REDIS_NOTICE 2
#define REDIS_WARNING 3

/* Anti-warning macro... */
#define REDIS_NOTUSED(V) ((void) V)

/* We can print the stacktrace, so our assert is defined this way: */
#define redisAssert(_e) ((_e) ? (void) 0 : (_redisAssert(#_e, __FILE__, __LINE__), _exit(1)))
static void _redisAssert(char *estr, char *file, int line);

/*================================= Data types ============================== */

/* The actual Redis Object */
typedef struct redisObject {
    void *ptr;
    unsigned char type;
    unsigned char encoding;
    int refcount;
} robj;

typedef struct redisDb {
    dict *dict;                 /* The keyspace for this DB */
    dict *expires;              /* Timeout of keys with a timeout set */
    dict *blockingkeys;         /* Keys with clients waiting for data (BLPOP) */
    dict *io_keys;              /* Keys with clients waiting for VM I/O */
    int id;
} redisDb;

/* With multiplexing we need to take per-clinet state.
 * Clients are taken in a liked list. */
typedef struct redisClient {
    int fd;
    redisDb *db;
    sds querybuf;
    robj **argv;
    int argc;
    int bulklen;            /* bulk read len. -1 if not in bulk read mode */
    list *reply;
    int sentlen;
    time_t lastinteraction; /* time of the last interaction, used for timeout */
    int flags;              /* REDIS_SLAVE | REDIS_MONITOR | REDIS_MULTI ... */
} redisClient;

/* Global server state structure */
struct redisServer {
    int port;
    int fd;
    redisDb *db;
    long long dirty;            /* changes to DB from the last save */
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
    int dbnum;
    char *logfile;
    char *bindaddr;
};

typedef void redisCommandProc(redisClient *c);
struct redisCommand {
    char *name;
    redisCommandProc *proc;
    int arity;
    int flags;
    /* Use a function to determine which keys need to be loaded
     * in the background prior to executing this command. Takes precedence
     * over vm_firstkey and others, ignored when NULL */
    redisCommandProc *vm_preload_proc;
    /* What keys should be loaded in background when calling this command? */
    int vm_firstkey; /* The first argument that's a key (0 = no keys) */
    int vm_lastkey;  /* THe last argument that's a key */
    int vm_keystep;  /* The step between first and last key */
};

/* Our shared "common" objects */

struct sharedObjectsStruct {
    robj *crlf, *ok, *err, *emptybulk, *czero, *cone, *pong, *space,
    *colon, *nullbulk, *nullmultibulk, *queued,
    *emptymultibulk, *wrongtypeerr, *nokeyerr, *syntaxerr, *sameobjecterr,
    *outofrangeerr, *plus,
    *select0, *select1, *select2, *select3, *select4,
    *select5, *select6, *select7, *select8, *select9;
} shared;

/*================================ Prototypes =============================== */

static void freeStringObject(robj *o);
static void decrRefCount(void *o);
static robj *createObject(int type, void *ptr);
static void freeClient(redisClient *c);
static void addReply(redisClient *c, robj *obj);
static void addReplySds(redisClient *c, sds s);
static void incrRefCount(robj *o);
static robj *createStringObject(char *ptr, size_t len);
static robj *getDecodedObject(robj *o);
static int removeExpire(redisDb *db, robj *key);
static int expireIfNeeded(redisDb *db, robj *key);
static int deleteKey(redisDb *db, robj *key);
static time_t getExpire(redisDb *db, robj *key);
static int setExpire(redisDb *db, robj *key, time_t when);
static int processCommand(redisClient *c);
static void processInputBuffer(redisClient *c);
static void acceptHandler(aeEventLoop *el, int fd, void *privdata, int mask);
static void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask);
static struct redisCommand *lookupCommand(char *name);
static void call(redisClient *c, struct redisCommand *cmd);
static void resetClient(redisClient *c);

static void pingCommand(redisClient *c);
static void setCommand(redisClient *c);
static void setnxCommand(redisClient *c);
static void getCommand(redisClient *c);
static void delCommand(redisClient *c);
static void existsCommand(redisClient *c);
static void expireCommand(redisClient *c);
static void expireatCommand(redisClient *c);
static void ttlCommand(redisClient *c);

/*================================= Globals ================================= */

/* Global vars */
static struct redisServer server; /* server global state */
static struct redisCommand cmdTable[] = {
    {"get", getCommand, 2, REDIS_CMD_INLINE, NULL, 0, 0, 0},
    {"set", setCommand, 3, REDIS_CMD_BULK|REDIS_CMD_DENYOOM, NULL, 0, 0, 0},
    // {"setnx", setnxCommand, 3, REDIS_CMD_BULK|REDIS_CMD_DENYOOM, NULL, 0, 0, 0},
    {"del", delCommand, -2, REDIS_CMD_INLINE, NULL, 0, 0, 0},
    {"exists", existsCommand, 2, REDIS_CMD_INLINE, NULL, 0, 0, 0},
    {"expire", expireCommand, 3, REDIS_CMD_INLINE, NULL, 0, 0, 0},
    {"expireat", expireatCommand, 3, REDIS_CMD_INLINE, NULL, 0, 0, 0},
    {"ping", pingCommand, 1, REDIS_CMD_INLINE, NULL, 0, 0, 0},
    {"ttl", ttlCommand, 2, REDIS_CMD_INLINE, NULL, 0, 0, 0},
    {NULL, NULL, 0, 0, NULL, 0, 0, 0}
};

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

/*====================== Hash table type implementation  ==================== */

static int sdsDictKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    int l1, l2;
    DICT_NOTUSED(privdata);

    l1 = sdslen((sds) key1);
    l2 = sdslen((sds) key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

static void dictRedisObjectDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);

    if (val == NULL) return; /* Values of swapped out keys as set to NULL */
    decrRefCount(val);
}

static int dictObjKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    const robj *o1 = key1, *o2 = key2;
    return sdsDictKeyCompare(privdata, o1->ptr, o2->ptr);
}

static unsigned int dictObjHash(const void *key) {
    const robj *o = key;
    return dictGenHashFunction(o->ptr, sdslen(o->ptr));
}

/* Db->dict */
static dictType dbDictType = {
    dictObjHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictObjKeyCompare,          /* key compare */
    dictRedisObjectDestructor,  /* key destructor */
    dictRedisObjectDestructor   /* val destructor */
};

/* Db->expires */
static dictType keyptrDictType = {
    dictObjHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictObjKeyCompare,          /* key compare */
    dictRedisObjectDestructor,  /* key destructor */
    NULL                        /* val destructor */
};

/* ========================= Random utility functions ======================= */

/* Redis generally does not try to recover from out of memory conditions
 * when allocating objects or strings, it is not clear if it will be possible
 * to report this condition to the client since the networking layer itself
 * is based on heap allocation for send buffers, so we simply abort.
 * At least the code will be simpler to read... */
static void oom(const char *msg) {
    redisLog(REDIS_WARNING, "%s: Out of memory\n",msg);
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
    // server.unixtime = time(NULL);

    /* Show some info about non-empty databases */
    for (j = 0; j < server.dbnum; j++) {
        long long size, used, vkeys;

        size = dictSlots(server.db[j].dict);
        used = dictSize(server.db[j].dict);
        vkeys = dictSize(server.db[j].expires);
        if (!(loops % 5) && (used || vkeys)) {
            redisLog(REDIS_VERBOSE, "DB %d: %lld keys (%lld volatile) in %lld slots HT.", j, used, vkeys, size);
            /* dictPrintStats(server.dict); */
        }
    }

    /* Show information about connected clients */
    if (!(loops % 5)) {
        redisLog(REDIS_VERBOSE, "%d clients connected, %zu bytes in use",
            listLength(server.clients),
            zmalloc_used_memory());
    }
    
    return 1000;
}

static void createSharedObjects(void) {
    shared.crlf = createObject(REDIS_STRING, sdsnew("\r\n"));
    shared.ok = createObject(REDIS_STRING, sdsnew("+OK\r\n"));
    shared.err = createObject(REDIS_STRING, sdsnew("-ERR\r\n"));
    shared.emptybulk = createObject(REDIS_STRING, sdsnew("$0\r\n\r\n"));
    shared.czero = createObject(REDIS_STRING, sdsnew(":0\r\n"));
    shared.cone = createObject(REDIS_STRING, sdsnew(":1\r\n"));
    shared.nullbulk = createObject(REDIS_STRING, sdsnew("$-1\r\n"));
    shared.nullmultibulk = createObject(REDIS_STRING, sdsnew("*-1\r\n"));
    shared.emptymultibulk = createObject(REDIS_STRING, sdsnew("*0\r\n"));
    shared.pong = createObject(REDIS_STRING, sdsnew("+PONG\r\n"));
}

static void initServerConfig() {
    server.dbnum = REDIS_DEFAULT_DBNUM;
    server.port = REDIS_SERVERPORT;
    server.verbosity = REDIS_DEBUG;
    server.logfile = NULL; /* NULL = log on standard output */
    server.bindaddr = NULL;
}

static void initServer() {
    int j;

    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    server.clients = listCreate();
    createSharedObjects();
    server.el = aeCreateEventLoop();
    server.db = zmalloc(sizeof(redisDb) * server.dbnum);
    server.fd = anetTcpServer(server.neterr, server.port, server.bindaddr);
    if (server.fd == -1) {
        redisLog(REDIS_WARNING, "Opening TCP port: %s", server.neterr);
        exit(1);
    }
    for (j = 0; j < server.dbnum; j++) {
        server.db[j].dict = dictCreate(&dbDictType, NULL);
        server.db[j].expires = dictCreate(&keyptrDictType, NULL);
        server.db[j].id = j;
    }
    server.cronloops = 0;
    server.stat_numcommands = 0;
    server.stat_numconnections = 0;
    server.stat_starttime = time(NULL);
    aeCreateTimeEvent(server.el, 1, serverCron, NULL, NULL);
    if (aeCreateFileEvent(server.el, server.fd, AE_READABLE,
        acceptHandler, NULL) == AE_ERR) oom("creating file event");
}

static void freeClientArgv(redisClient *c) {
    int j;

    for (j = 0; j < c->argc; j++)
        decrRefCount(c->argv[j]);
    c->argc = 0;
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
    freeClientArgv(c);
    close(c->fd);
    /* Remove from the list of clients */
    ln = listSearchKey(server.clients, c);
    redisAssert(ln != NULL);
    listDelNode(server.clients, ln);
    zfree(c->argv);
    zfree(c);
}

static void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    redisClient *c = privdata;
    int nwritten = 0, totwritten = 0, objlen;
    robj *o;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);

    while (listLength(c->reply)) {
        o = listNodeValue(listFirst(c->reply));
        objlen = sdslen(o->ptr);

        if (objlen == 0) {
            listDelNode(c->reply, listFirst(c->reply));
            continue;
        }

        nwritten = write(fd, (char *) o->ptr + c->sentlen, objlen - c->sentlen);
        if (nwritten <= 0) break;

        c->sentlen += nwritten;
        totwritten += nwritten;
        /* If we fully sent the object on head go to the next one */
        if (c->sentlen == objlen) {
            listDelNode(c->reply, listFirst(c->reply));
            c->sentlen = 0;
        }
        /* Note that we avoid to send more thank REDIS_MAX_WRITE_PER_EVENT
         * bytes, in a single threaded server it's a good idea to serve
         * other clients as well, even if a very large request comes from
         * super fast link that is always able to accept data (in real world
         * scenario think about 'KEYS *' against the loopback interfae) */
        if (totwritten > REDIS_MAX_WRITE_PER_EVENT) break;
    }
    if (nwritten == -1) {
        if (errno == EAGAIN) {
            nwritten = 0;
        } else {
            redisLog(REDIS_VERBOSE,
                "Error writing to client: %s", strerror(errno));
            freeClient(c);
            return;
        }
    }
    if (totwritten > 0) c->lastinteraction = time(NULL);
    if (listLength(c->reply) == 0) {
        c->sentlen = 0;
        aeDeleteFileEvent(server.el, c->fd, AE_WRITABLE);
    }
}

static struct redisCommand *lookupCommand(char *name) {
    int j;
    while (cmdTable[j].name != NULL) {
        if (!strcasecmp(name, cmdTable[j].name)) return &cmdTable[j];
        j++;
    }
    return NULL;
}

/* resetClient prepare the client to process the next command */
static void resetClient(redisClient *c) {
    freeClientArgv(c);
    c->bulklen = -1;
    // c->multibulk = 0;
}

/* Call() is the core of Redis execution of a command */
static void call(redisClient *c, struct redisCommand *cmd) {
    cmd->proc(c);
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
    struct redisCommand *cmd;

    /* The QUIT command is handled as a special case. Normal command
     * procs are unable to close the client connection safely */
    if (!strcasecmp(c->argv[0]->ptr, "quit")) {
        freeClient(c);
        return 0;
    }

    /* Now lookup the command and check ASAP about trivial error conditions
     * such wrong arity, bad command name and so forth. */
    cmd = lookupCommand(c->argv[0]->ptr);
    if (!cmd) {
        addReplySds(c,
            sdscatprintf(sdsempty(), "-ERR unknown command '%s'\r\n",
                (char *) c->argv[0]->ptr));
        resetClient(c);
        return 1;
    } else if ((cmd->arity > 0 && cmd->arity != c->argc) ||
               (c->argc < -cmd->arity)) {
        addReplySds(c,
            sdscatprintf(sdsempty(),
                "-ERR wrong number of arguments for '%s' command\r\n",
                cmd->name));
        resetClient(c);
        return 1;
    }

    call(c, cmd);

    /* Prepare the client for the next command */
    resetClient(c);
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
            *p = '\0'; /* remove "\n" */
            if (*(p-1) == '\r') *(p-1) = '\0'; /* and "\r" if any */
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
    }
}

static void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    redisClient *c = (redisClient*) privdata;
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

static int selectDb(redisClient *c, int id) {
    if (id < 0 || id >= server.dbnum)
        return REDIS_ERR;
    c->db = &server.db[id];
    return DICT_OK;
}

static void *dupClientReplyValue(void *o) {
    incrRefCount((robj *) o);
    return o;
}

static redisClient *createClient(int fd) {
    redisClient *c = zmalloc(sizeof(*c));

    anetNonBlock(NULL, fd);
    anetTcpNoDelay(NULL, fd);
    if (!c) return NULL;
    selectDb(c, 0);
    c->fd = fd;
    c->querybuf = sdsempty();
    c->argc = 0;
    c->argv = NULL;
    c->bulklen = -1;
    c->sentlen = 0;
    c->flags = 0;
    c->lastinteraction = time(NULL);
    c->reply = listCreate();
    listSetFreeMethod(c->reply, decrRefCount);
    listSetDupMethod(c->reply, dupClientReplyValue);
    if (aeCreateFileEvent(server.el, c->fd, AE_READABLE,
        readQueryFromClient, c) == AE_ERR) {
        freeClient(c);
        return NULL;
    }
    listAddNodeTail(server.clients, c);
    return c;
}

static void addReply(redisClient *c, robj *obj) {
    if (listLength(c->reply) == 0 &&
        aeCreateFileEvent(server.el, c->fd, AE_WRITABLE,
            sendReplyToClient, c) == AE_ERR) return;
    listAddNodeTail(c->reply, getDecodedObject(obj));
}

static void addReplySds(redisClient *c, sds s) {
    robj *o = createObject(REDIS_STRING, s);
    addReply(c, o);
    decrRefCount(o);
}

static void addReplyDouble(redisClient *c, double d) {
    char buf[128];

    snprintf(buf, sizeof(buf), "%.17g", d);
    addReplySds(c, sdscatprintf(sdsempty(), "$%lu\r\n%s\r\n",
        (unsigned long) strlen(buf), buf));
}

static void addReplyLong(redisClient *c, long l) {
    char buf[128];
    size_t len;

    if (l == 0) {
        addReply(c, shared.czero);
        return;
    } else if (l == 1) {
        addReply(c, shared.cone);
        return;
    }
    len = snprintf(buf, sizeof(buf), ":%ld\r\n", l);
    addReplySds(c, sdsnewlen(buf, len));
}

static void addReplyUlong(redisClient *c, unsigned long ul) {
    char buf[128];
    size_t len;

    if (ul == 0) {
        addReply(c, shared.czero);
        return;
    } else if (ul == 1) {
        addReply(c, shared.cone);
        return;
    }
    len = snprintf(buf, sizeof(buf), ":%lu\r\n", ul);
    addReplySds(c, sdsnewlen(buf, len));
}

static void addReplyBulkLen(redisClient *c, robj *obj) {
    size_t len;

    if (obj->encoding == REDIS_ENCODING_RAW) {
        len = sdslen(obj->ptr);
    }
    addReplySds(c, sdscatprintf(sdsempty(), "$%lu\r\n", (unsigned long) len));
}

static void addReplyBulk(redisClient *c, robj *obj) {
    addReplyBulkLen(c, obj);
    addReply(c, obj);
    addReply(c, shared.crlf);
}

static void acceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd;
    char cip[128];
    redisClient *c;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);
    REDIS_NOTUSED(privdata);

    cfd = anetAccept(server.neterr, fd, cip, &cport);
    if (cfd == ANET_ERR) {
        redisLog(REDIS_VERBOSE, "Accepting client connection: %s", server.neterr);
        return;
    }
    redisLog(REDIS_VERBOSE, "Accepted %s:%d", cip, cport);
    if ((c = createClient(cfd)) == NULL) {
        redisLog(REDIS_WARNING, "Error allocating resources for the client");
        close(cfd); /* May be already closed, just ignore errors */
        return;
    }
    server.stat_numconnections++;
}

/* ======================= Redis objects implementation ===================== */

static robj *createObject(int type, void *ptr) {
    robj *o = zmalloc(sizeof(*o));

    o->type = type;
    o->encoding = REDIS_ENCODING_RAW;
    o->ptr = ptr;
    o->refcount = 1;

    return o;
}

static void freeStringObject(robj *o) {
    if (o->encoding == REDIS_ENCODING_RAW) {
        sdsfree(o->ptr);
    }
}

static void incrRefCount(robj *o) {
    o->refcount++;
}

static void decrRefCount(void *obj) {
    robj *o = obj;

    /* Object is in memory, or in the process of being swapped out. */
    if (--o->refcount == 0) {
        switch (o->type) {
        case REDIS_STRING: freeStringObject(o); break;
        default: redisAssert(0); break;
        }
    }
}

static robj *lookupKey(redisDb *db, robj *key) {
    dictEntry *de = dictFind(db->dict, key);
    if (de) {
        robj *key = dictGetEntryKey(de);
        robj *val = dictGetEntryVal(de);

        return val;
    } else {
        return NULL;
    }
}

static robj *lookupKeyRead(redisDb *db, robj *key) {
    expireIfNeeded(db, key);
    return lookupKey(db, key);
}

static robj *lookupKeyReadOrReply(redisClient *c, robj *key, robj *reply) {
    robj *o = lookupKeyRead(c->db, key);
    if (!o) addReply(c, reply);
    return o;
}

static int deleteKey(redisDb *db, robj *key) {
    int retval;

    /* We need to protect key from destruction: after the first dictDelete()
     * it may happen that 'key' is no longer valid if we don't increment
     * it's count. This may happen when we get the object reference directly
     * from the hash table with dictRandomKey() or dict iterators */
    // incrRefCount(key);
    retval = dictDelete(db->dict, key);
    // decrRefCount(key);

    return retval == DICT_OK;
}

/* Get a decoded version of an encoded object (returned as a new object).
 * If the object is already raw-encoded just increment the ref count. */
static robj *getDecodedObject(robj *o) {
    if (o->encoding == REDIS_ENCODING_RAW) {
        incrRefCount(o);
        return o;
    }
    return o;
}

/*================================== Commands =============================== */

static void pingCommand(redisClient *c) {
    addReply(c, shared.pong);
}

/*=================================== Strings =============================== */

static void setGenericCommand(redisClient *c, int nx) {
    int retval;

    retval = dictAdd(c->db->dict, c->argv[1], c->argv[2]);
    if (retval == DICT_ERR) {
        addReply(c, shared.czero);
        return;
    } else {
        incrRefCount(c->argv[1]);
        incrRefCount(c->argv[2]);
    }
    addReply(c, shared.ok);
}

static void setCommand(redisClient *c) {
    setGenericCommand(c, 0);
}

static int getGenericCommand(redisClient *c) {
    robj *o;

    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.nullbulk)) == NULL)
        return REDIS_OK;
    
    addReplyBulk(c, o);
    return REDIS_OK;
}

static void getCommand(redisClient *c) {
    getGenericCommand(c);
}

/* ========================= Type agnostic commands ========================= */

static void delCommand(redisClient *c) {
    int deleted = 0, j;

    for (j = 1; j < c->argc; j++) {
        if (deleteKey(c->db, c->argv[j])) {
            server.dirty++;
            deleted++;
        }
    }
    addReplyLong(c, deleted);
}

/* ================================= Expire ================================= */

static int setExpire(redisDb *db, robj *key, time_t when) {
    if (dictAdd(db->expires, key, (void *) when) == DICT_ERR) {
        return 0;
    } else {
        incrRefCount(key);
        return 1;
    }
}

/* Return the expire time of the specified key, or -1 if no expire
 * is associated with this key (i.e. the key is non volatile) */
static time_t getExpire(redisDb *db, robj *key) {
    dictEntry *de;

    /* No expire? return ASAP */
    if (dictSize(db->expires) == 0 ||
        (de = dictFind(db->expires, key)) == NULL) return -1;
    
    return (time_t) dictGetEntryVal(de);
}

static int expireIfNeeded(redisDb *db, robj *key) {
    time_t when;
    dictEntry *de;

    /* No expire? return ASAP */
    if (dictSize(db->expires) == 0 ||
        (de = dictFind(db->expires, key)) == NULL) return 0;
    
    /* Lookup the expire */
    when = (time_t) dictGetEntryVal(de);
    if (time(NULL) <= when) return 0;

    /* Delete the key */
    dictDelete(db->expires, key);
    return dictDelete(db->dict, key) == DICT_OK;
}

static void expireGenericCommand(redisClient *c, robj *key, time_t seconds) {
    dictEntry *de;

    de = dictFind(c->db->dict, key);
    if (de == NULL) {
        addReply(c, shared.czero);
        return;
    }
    if (seconds < 0) {
        if (deleteKey(c->db, key)) server.dirty++;
        addReply(c, shared.cone);
        return;
    } else {
        time_t when = time(NULL) + seconds;
        if (setExpire(c->db, key, when)) {
            addReply(c, shared.cone);
            server.dirty++;
        } else {
            addReply(c, shared.czero);
        }
        return;
    }
}

static void expireCommand(redisClient *c) {
    expireGenericCommand(c, c->argv[1], strtol(c->argv[2]->ptr, NULL , 10));
}

static void expireatCommand(redisClient *c) {
    expireGenericCommand(c, c->argv[1], strtol(c->argv[2]->ptr, NULL , 10) - time(NULL));
}

static void ttlCommand(redisClient *c) {
    time_t expire;
    int ttl = -1;

    expire = getExpire(c->db, c->argv[1]);
    if (expire != -1) {
        ttl = (int) (expire - time(NULL));
        if (ttl < 0) ttl = -1;
    }
    addReplySds(c, sdscatprintf(sdsempty(), ":%d\r\n", ttl));
}

static void existsCommand(redisClient *c) {
    addReply(c, lookupKeyRead(c->db, c->argv[1]) ? shared.cone : shared.czero);
}

static void _redisAssert(char *estr, char *file, int line) {
    redisLog(REDIS_WARNING, "=== ASSERTION FAILED ===");
    redisLog(REDIS_WARNING, "==> %s:%d '%s' is not true\n", file, line, estr);
#ifdef HAVE_BACKTRACE
    redisLog(REDIS_WARNING, "(forcing SIGSEGV in order to print the stack trace)");
    *((char *) -1) = 'x';
#endif
}

/* =================================== Main! ================================ */

int main(int argc, char **argv)
{
    initServerConfig();
    if (argc == 2) {

    } else if (argc > 2) {
        fprintf(stderr, "Usage: ./redis-server [/path/to/redis.conf]\n");
        exit(1);
    } else {
        redisLog(REDIS_WARNING, "Warning: no config file specified, using the default config. In order to specify a config file use 'redis-server /path/to/redis.conf'");
    }
    initServer();
    redisLog(REDIS_NOTICE, "Server started, Redis version " REDIS_VERSION);
    redisLog(REDIS_NOTICE, "The server is now ready to accept connections on port %d", server.port);
    aeMain(server.el);
    aeDeleteEventLoop(server.el);
    return 0;
}
