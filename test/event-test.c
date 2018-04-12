#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "zmalloc.h"
#include "ae.h"

static void
fifo_read(aeEventLoop *el, int fd, void *arg, int mask)
{
    fprintf(stderr, "call fifo_read: el: %p, fd: %d, arg: %p, mask: %d\n",
            el, fd, arg, mask);
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

    fd = open(fifo, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("open error");
        exit(EXIT_FAILURE);
    }

    el = aeCreateEventLoop();

    // aeCreateFileEvent(el, fd, AE_READABLE, fifo_read, NULL);

    exit(EXIT_SUCCESS);
}
