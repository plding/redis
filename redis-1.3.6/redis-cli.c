/* Redis CLI (command line interface)
 *
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

#include "fmacros.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "anet.h"
#include "sds.h"
// #include "adlist.h"
#include "zmalloc.h"

#define REDIS_CMD_INLINE    1
#define REDIS_CMD_BULK      2
#define REDIS_CMD_MULTIBULK 4

#define REDIS_NOTUSED(V) ((void) V)

static struct config {
    char *hostip;
    int hostport;
    int dbnum;
} config;

struct redisCommand {
    char *name;
    int arity;
    int flags;
};

static struct redisCommand cmdTable[] = {
    {"ping", 1, REDIS_CMD_INLINE},
    {NULL, 0, 0}
};

static struct redisCommand *lookupCommand(char *name) {
    int j = 0;
    while (cmdTable[j].name != NULL) {
        if (!strcasecmp(name, cmdTable[j].name)) return &cmdTable[j];
        j++;
    }
    return NULL;
}

static int cliConnect(void) {
    char err[ANET_ERR_LEN];
    static int fd = ANET_ERR;

    if (fd == ANET_ERR) {
        fd = anetTcpConnect(err, config.hostip, config.hostport);
        if (fd == ANET_ERR) {
            fprintf(stderr, "Could not connect to Redis at %s:%d: %s\n", config.hostip, config.hostport, err);
            return -1;
        }
        anetTcpNoDelay(NULL, fd);
    }
    return fd;
}

static sds cliReadLine(int fd) {
    sds line = sdsempty();

    while (1) {
        char c;
        ssize_t ret;

        ret = read(fd, &c, 1);
        if (ret == -1) {
            sdsfree(line);
            return NULL;
        } else if (ret == 0 || c == '\n') {
            break;
        } else {
            line = sdscatlen(line, &c, 1);
        }
    }
    return line;
}

static int cliReadSingleLineReply(int fd, int quiet) {
    sds reply = cliReadLine(fd);

    if (reply == NULL) return 1;
    if (!quiet)
        printf("%s\n", reply);
    sdsfree(reply);
    return 0;
}

static int cliReadReply(int fd) {
    char type;

    if (anetRead(fd, &type, 1) <= 0) exit(1);
    switch (type) {
    case '-':
        printf("(error) ");
        return 1;
    case '+':
        return cliReadSingleLineReply(fd, 0);
    default:
        printf("protocol error, got '%c' as reply type byte\n", type);
        return 1;
    }
}

static int selectDb(int fd) {
    if (config.dbnum == 0)
        return 0;

    return 1;
}

static int cliSendCommand(int argc, char **argv) {
    struct redisCommand *rc = lookupCommand(argv[0]);
    int fd, j, retval = 0;
    sds cmd;

    if (!rc) {
        fprintf(stderr, "Unknown command '%s'\n", argv[0]);
        return 1;
    }

    if ((rc->arity > 0 && argc != rc->arity) ||
        (rc->arity < 0 && argc < -rc->arity)) {
        fprintf(stderr, "Wrong number of arguments for '%s'\n", rc->name);
        return 1;
    }
    if ((fd = cliConnect()) == -1) return 1;

    /* Select db number */
    retval = selectDb(fd);
    if (retval) {
        fprintf(stderr, "Error setting DB num\n");
        return 1;
    }

    cmd = sdsempty();
    for (j = 0; j < argc; j++) {
        if (j != 0) cmd = sdscat(cmd, " ");
        cmd = sdscatlen(cmd, argv[j], sdslen(argv[j]));
    }
    cmd = sdscat(cmd, "\r\n");

    anetWrite(fd, cmd, sdslen(cmd));
    sdsfree(cmd);

    retval = cliReadReply(fd);
    if (retval) {
        return retval;
    }

    return 0;
}

static int parseOptions(int argc, char **argv) {
    // int i;

    // for (i = 1; i < argc; i++) {
    //     int lastarg = i == argc-1;

    //     if (!strcmp(argv[i], "-h") && !lastarg) {
    //         char *ip = zmalloc(32);
    //         if (anetResolve(NULL, argv[i+1], ip) == ANET_ERR) {
    //             printf("Can't resolve %s\n", argv[i]);
    //             exit(1);
    //         }
    //         config.hostip = ip;
    //         i++;
    //     } else if (!strcmp(argv[i], "-h") && lastarg) {
    //         usage();
    //     } else if (!strcmp(argv[i], "-p") && !lastarg) {
    //         config.hostport = atoi(argv[i+1]);
    //         i++;
    //     } else {
    //         break;
    //     }
    // }
    // return i;
}

/* Turn the plain C strings into Sds strings */
static char **convertToSds(int count, char **args) {
    int j;
    char **sds = zmalloc(sizeof(sds) * count);

    for (j = 0; j < count; j++)
        sds[j] = sdsnew(args[j]);

    return sds;
}

static char *prompt(char *line, int size) {
    char *retval;

    do {
        printf(">> ");
        retval = fgets(line, size, stdin);
    } while (retval && *line == '\n');
    line[strlen(line) - 1] = '\0';

    return retval;
}

static void repl() {
    int size = 4096, max = size >> 1, argc;
    char buffer[size];
    char *line = buffer;
    char **ap, *args[max];

    while (prompt(line, size)) {
        argc = 0;

        for (ap = args; (*ap = strsep(&line, " \t")) != NULL;) {
            if (**ap != '\0') {
                if (argc >= max) break;
                if (strcasecmp(*ap, "quit") == 0 || strcasecmp(*ap, "exit") == 0)
                    exit(0);
                ap++;
                argc++;
            }
        }

        cliSendCommand(argc, convertToSds(argc, args));
        line = buffer;
    }

    exit(0);
}

int main(int argc, char **argv) {
    config.hostip = "127.0.0.1";
    config.hostport = 6379;
    config.dbnum = 0;

    repl();
}
