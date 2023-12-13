int domain_prepare(int epoll_fd) {
    printf("... by socket: echo 2 | nc -U socket\n");
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) die("socket");

    char *sock_name = "socket";
    struct sockaddr_un server_sockaddr;
    server_sockaddr.sun_family = AF_UNIX;
    strcpy(server_sockaddr.sun_path, sock_name);

    int rc = unlink(sock_name);
    if (rc < 0 && errno != ENOENT) die("unlink/socket");

    rc = bind(sock_fd, (struct sockaddr *) &server_sockaddr, sizeof(server_sockaddr));
    if (rc == -1) die("bind/socket");

    rc = listen(sock_fd, 10);
    if (rc < 0) die("listen/socket");

    epoll_add(epoll_fd, sock_fd, EPOLLIN);

    return sock_fd;
}


void domain_accept(int epoll_fd, int sock_fd, int events) {
    int client_fd = accept(sock_fd, NULL, NULL);
    epoll_add(epoll_fd, client_fd, EPOLLIN);
}


void domain_recv(int epoll_fd, int sock_fd, int events) {
    static char buf[128];
    if (events & EPOLLIN) {
        int len = recv(sock_fd, buf, sizeof(buf), 0);
        if (len < 0) die("recvfrom");

        while (len > 0 && buf[len-1] == '\n') len --;
        buf[len] = 0;

        struct ucred ucred;
        {
            socklen_t len = sizeof(struct ucred);
            int rc = getsockopt(sock_fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len);
            if (rc < 0) die("getsockopt");
        }

        printf("socket[pid=%d,uid=%d,gid=%d]: %s\n", ucred.pid, ucred.uid, ucred.gid, buf);
        epoll_del(epoll_fd, sock_fd);
        close(sock_fd);
    } else if (events & EPOLLHUP) {
        epoll_del(epoll_fd, sock_fd);
        close(sock_fd);
    }
}
