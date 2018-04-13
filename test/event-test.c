#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#include "zmalloc.h"
#include "ae.h"

static void
fifo_read(aeEventLoop *el, int fd, void *arg, int mask)
{
    ssize_t nread;
    char buf[255];

    fprintf(stderr, "call fifo_read: el: %p, fd: %d, arg: %p, mask: %d\n",
            el, fd, arg, mask);

    nread = read(fd, buf, sizeof(buf));
    if (nread < 0) {
        if (errno != EINTR) {
            perror("read error");
            exit(EXIT_FAILURE);
        }

        return;

    } else if (nread == 0) {
        fprintf(stderr, "connection closed\n");
        exit(EXIT_FAILURE);
    }

    buf[nread] = 0;
    fputs(buf, stdout);
}


int
main(void)
{
    aeEventLoop *el;
    char *fifo = "event.fifo";
    int fd;

    unlink(fifo);

    if (mkfifo(fifo, 0666) < 0) {
        perror("mkfifo error");
        exit(EXIT_FAILURE);
    }

#ifdef __linux
    fd = open(fifo, O_RDWR | O_NONBLOCK);
#else
    fd = open(fifo, O_RDONLY | O_NONBLOCK);
#endif
    
    if (fd < 0) {
        perror("open error");
        exit(EXIT_FAILURE);
    }

    el = aeCreateEventLoop();

    fprintf(stderr, "use api name: %s\n", aeGetApiName());

    aeCreateFileEvent(el, fd, AE_READABLE, fifo_read, NULL);

    aeMain(el);

    exit(EXIT_SUCCESS);
}
