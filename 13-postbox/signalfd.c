int signalfd_prepare(int epoll_fd) {
    printf("... by signal: /bin/kill -USR1 -q 3 %d \n", getpid());
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGUSR2);

    int signal_fd = signalfd(-1, &mask, 0);
    if (signal_fd < 0) die("signal_fd");

    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1)
        die("sigprocmask");

    epoll_add(epoll_fd, signal_fd, EPOLLIN);

    return signal_fd;
}

void signalfd_handle(int epoll_fd, int signal_fd, int events) {
    struct signalfd_siginfo info;
    if (events & EPOLLIN) {
        int len = read(signal_fd, &info, sizeof(info));
        if (len != sizeof(info )) die("read");
    } else {
        die("invalid singal on signal_fd");
    }

    printf("signalfd[uid=%d,pid=%d] signal=%d, data=%x\n",
           info.ssi_pid,
           info.ssi_uid,
           info.ssi_signo,
           info.ssi_int
        );
}
