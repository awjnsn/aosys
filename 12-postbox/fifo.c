static int open_fifo() {
    int fd = open("fifo", O_RDONLY|O_NONBLOCK);
    if (fd < 0) die("open/fifo");
    return fd;
}

int fifo_prepare(int epoll_fd) {
    printf("... by fifo:   echo 1 > fifo\n");
    int rc = unlink("fifo");
    if (rc < 0 && errno != ENOENT) die("unlink/fifo");
    rc = mknod("fifo",  0666 | S_IFIFO, 0);
    if (rc < 0) die("mknod/fifo");

    int fifo_fd = open_fifo();
    epoll_add(epoll_fd, fifo_fd, EPOLLIN);

    return fifo_fd;
}

void fifo_handle(int epoll_fd, int fifo_fd, int events) {
    static char buf[128];

    if (events & EPOLLIN) {
        int len = read(fifo_fd, buf, sizeof(buf));
        if (len < 0) die("read/fifo");
        if (len == 0)
            goto close;
        while (len > 1 && buf[len-1] == '\n') len --;
        buf[len] = 0;
        printf("fifo: %s\n", buf);
    } else if (events & EPOLLHUP) {
    close:
        epoll_del(epoll_fd, fifo_fd);
        close(fifo_fd);

        fifo_fd = open_fifo();
        epoll_add(epoll_fd, fifo_fd, EPOLLIN);
    }
}
