int mqueue_prepare(int epoll_fd) {
    printf("... by mq_send: ./mq_send 4 (see also `cat /dev/mqueue/postbox`)\n");
    struct mq_attr attr;
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = 128;
    int msg_fd = mq_open("/postbox", O_RDONLY | O_CREAT, 0666, &attr);
    if (msg_fd < 0) die("mq_open");

    epoll_add(epoll_fd, msg_fd, EPOLLIN);

    return msg_fd;
}

void mqueue_handle(int epoll_fd, int msg_fd, int events) {
    unsigned int prio;
    static char buf[128];
    int len = mq_receive(msg_fd, buf, sizeof(buf), &prio);
    if (len < 0) die("mq_receive");

    while (len > 0 && buf[len-1] == '\n') len --;
    buf[len] = 0;

    printf("mqueue[prio=%d]: %s\n", prio, buf);
}

