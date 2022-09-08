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
#include "adlist.h"
#include "zmalloc.h"

#define REDIS_CMD_INLINE 1
#define REDIS_CMD_BULK 2
#define REDIS_CMD_MULTIBULK 4

#define REDIS_NOTUSED(V) ((void) V)

static struct config {
    char *hostip;
    int hostport;
    long repeat;
    int dbnum;
    int interactive;
    char *auth;
} config;

struct redisCommand {
    char *name;
    int arity;
    int flags;
};

static struct redisCommand cmdTable[] = {
    {"get", 2, REDIS_CMD_INLINE},
    {"set", 2, REDIS_CMD_BULK},
    {NULL, 0, 0}
};

static void usage();

static struct redisCommand *lookupCommand(char *name) {
    int j = 0;
    while (cmdTable[j].name != NULL) {
        if (!strcasecmp(name, cmdTable[j].name)) return &cmdTable[j];
        j++;
    }
    return NULL;
}

static int cliSendCommand(int argc, char **argv) {
    return 0;
}

static int parseOptions(int argc, char **argv) {
    int i;

    for (i = 1; i < argc; i++) {
        int lastarg = i == argc - 1;

        if (!strcmp(argv[i], "-h") && !lastarg) {
            char *ip = zmalloc(32);
            // if (anetResolve(NULL, argv[i+1], ip) == ANET_ERR) {
            //     printf("Can't resolve %s\n", argv[i]);
            //     exit(1);
            // }
            config.hostip = ip;
            i++;
        } else if (!strcmp(argv[i], "-h") && lastarg) {
            usage();
        } else if (!strcmp(argv[i], "-p") && !lastarg) {
            config.hostport = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i], "-r") && !lastarg) {
            config.repeat = strtoll(argv[i+1], NULL, 10);
            i++;
        } else if (!strcmp(argv[i], "-n") && !lastarg) {
            config.dbnum = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i], "-a") && !lastarg) {
            config.auth = argv[i+1];
            i++;
        } else if (!strcmp(argv[i], "-i")) {
            config.interactive = 1;
        } else {
            break;
        }
    }
    return i;
}

static void usage() {
    fprintf(stderr, "usage: redis-cli [-h host] [-p port] [-a authpw] [-r repeat_times] [-n db_num] [-i] cmd arg1 arg2 arg3 ... argN\n");
    fprintf(stderr, "usage: echo \"argN\" | redis-cli [-h host] [-a authpw] [-p port] [-r repeat_times] [-n db_num] cmd arg1 arg2 ... arg(N-1)\n");
    fprintf(stderr, "\nIf a pipe from standard input is detected this data is used as last argument.\n\n");
    fprintf(stderr, "example: cat /etc/passwd | redis-cli set my_passwd\n");
    fprintf(stderr, "example: redis-cli get my_passwd\n");
    fprintf(stderr, "example: redis-cli -r 100 lpush mylist x\n");
    fprintf(stderr, "\nRun in interactive mode: redis-cli -i or just don't pass any command\n");
    exit(1);
}

/* Turn the plain C strings into Sds strings */
static char **convertToSds(int count, char **args) {
    int j;
    char **sds = zmalloc(sizeof(char *) * count + 1);

    for (j = 0; j < count; j++)
        sds[j] = sdsnew(args[j]);
    
    return sds;
}

static void repl() {
    exit(0);
}

int main(int argc, char **argv) {
    int firstarg;
    char **argvcopy;
    struct redisCommand *rc;

    config.hostip = "127.0.0.1";
    config.hostport = 6379;
    config.repeat = 1;
    config.dbnum = 0;
    config.interactive = 0;
    config.auth = NULL;

    firstarg = parseOptions(argc, argv);
    argc -= firstarg;
    argv += firstarg;

    // if (argc == 0 || config.interactive == 1) repl();

    argvcopy = convertToSds(argc, argv);

    /* Read the last argument from stdandard input if needed */
    if ((rc = lookupCommand(argv[0])) != NULL) {
        if (rc->arity > 0 && argc == rc->arity - 1) {
            // sds lastarg = readArgFromStdin();
            // argvcopy[argc] = lastarg;
            // argc++;
        }
    }

    return cliSendCommand(argc, argvcopy);
}
