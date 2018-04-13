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
#include <signal.h>

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

#include "redis.h"
#include "ae.h"      /* Event driven programming library */
#include "zmalloc.h" /* total memory usage aware version of malloc/free */

/* Error codes */
#define REDIS_OK    0
#define REDIS_ERR  -1

/* Static server configuration */
#define REDIS_SERVERPORT    6379    /* TCP port */

/* Log levels */
#define REDIS_DEBUG     0
#define REDIS_VERBOSE   1
#define REDIS_NOTICE    2
#define REDIS_WARNING   3

/* Anti-warning macro... */
#define REDIS_NOTUSED(V) ((void) V)

/*================================= Data types ============================== */

/* Global server state structure */
struct redisServer {
    int port;
    int fd;
    aeEventLoop *el;
    int verbosity;
    int daemonize;
    char *pidfile;
    char *logfile;
    char *bindaddr;
};

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
        strftime(buf, 64, "%b %H:%M:%S", localtime(&now));
        fprintf(fp, "[%d] %s %c ", (int) getpid(), buf, c[level]);
        vfprintf(fp, fmt, ap);
        fprintf(fp, "\n");
        fflush(fp);
    }
    va_end(ap);

    if (server.logfile) fclose(fp);
}

static void initServerConfig() {
    server.port = REDIS_SERVERPORT;
    server.verbosity = REDIS_VERBOSE;
    server.logfile = NULL;  /* NULL = log on standard output */
    server.bindaddr = NULL;
    server.daemonize = 0;
    server.pidfile = "/var/run/redis.pid";
}

static void initServer() {
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    server.el = aeCreateEventLoop();
}

/* I agree, this is a very rudimental way to load a configuration...
   will improve later if the config gets more complex */
// static void loadServerConfig(char *filename) {
//     FILE *fp;
//     char buf[REDIS_CONFIG_MAX+1], *err = NULL;
//     int linenum = 0;
//     sds line = NULL;

//     if (filename[0] == '-' && filename[1] == '\0')
//         fp = stdin
//     else {
//         if ((fp = fopen(filename, "r")) == NULL) {
//             redisLog(REDIS_WARNING, "Fatal error, can't open config file");
//             exit(1);
//         }
//     }

//     while (gets(buf, REDIS_CONFIG_MAX+1, fp) != NULL) {
//         sds *argv;
//         int argc, j;

//         linenum++;
//         line = sdsnew(buf);
//         line = sdstrim(line, " \t\r\n");

//         /* Skip comments and blank lines */
//         if (line[0] == '#' || line[0] == '\0') {
//             sdsfree(line);
//             continue;
//         }

//         /* Split into arguments */
//         argv = sdssplitlen(line, sdslen(line), " ", 1, &argc);
//         sdstolower(argv[0]);

//         /* Execute config directives */
//         if (!strcasecmp(argv[0], "port") && argc == 2) {
//             server.port = atoi(argv[1]);
//             if (server.port < 1 || server.port > 65535) {
//                 err = "Invalid port"; goto loaderr;
//             }
//         } else if (!strcasecmp(argv[0], "bind") && argc == 2) {
//             server.bindaddr = zstrdup(argv[1]);
//         } else {
//             err = "Bad directive or wrong number of arguments"; goto loaderr;
//         }
//         for (j = 0; j < argc; j++)
//             sdsfree(argv[j]);
//         zfree(argv);
//         sdsfree(line);
//     }
//     if (fp != stdin) fclose(fp);
//     return;

// loaderr:
//     fprintf(stderr, "\n*** FATAL CONFIG FILE ERROR ***\n");
//     fprintf(stderr, "Reading the configuration file, at line %d\n", linenum);
//     fprintf(stderr, ">>> '%s'\n", line);
//     fprintf(stderr, "%s\n", err);
//     exit(1);
// }

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
    if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
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
    initServerConfig();
    if (argc == 2) {
        // loadServerConfig(argv[1]);
    } else if (argc > 2) {
        fprintf(stderr, "Usage: ./redis-server [/path/to/redis.conf]\n");
        exit(1);
    } else {
        redisLog(REDIS_WARNING, "Warning: no config file specified, using the default config. In order to specify a config file use 'redis-server /path/to/redis.conf'");
    }
    if (server.daemonize) daemonize();
    initServer();
    redisLog(REDIS_NOTICE, "Server started, Redis version " REDIS_VERSION);
#ifdef __linux__
    linuxOvercommitMemoryWarning();
#endif
    redisLog(REDIS_NOTICE, "The server is now ready to accept connections on port %d", server.port);
    aeMain(server.el);
    return 0;
}