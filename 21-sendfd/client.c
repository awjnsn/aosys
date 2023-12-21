#include <sys/socket.h>
#include <sys/signalfd.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <signal.h>


#define die(msg) do { perror(msg); exit(EXIT_FAILURE); } while(0)

// Receive a message with an attached file descriptor from a
// connected UNIX domain socket. The message is stored in the buffer,
// and the file descriptor in *fd. The function returns the length of
// the message.
//
// sock_fd:      Connected UNIX domain socket
// buf, bufsize: Buffer for the received message
// fd:           Where to store the file descriptor
int recvfd(int sock_fd, char *buf, size_t bufsize, int *fd) {
    // We prepare the msghdr like we do in the server, but do not
    // fill the cmsg of the msghdr. For details, please look at server.c
    struct iovec data = { .iov_base = buf, .iov_len = bufsize };
    union {
        char buf[CMSG_SPACE(sizeof(*fd))];
        struct cmsghdr align;
    } auxdata;

    struct msghdr msgh = {
        .msg_iov        = &data,
        .msg_iovlen     = 1,
        .msg_control    = auxdata.buf,
        .msg_controllen = sizeof(auxdata.buf),
    };

    // We received from our socket a message (with space for an
    // auxiliary cmsg). As we use SOCK_SEQPACKET, this will either receive the whole message or fail.
    int len = recvmsg(sock_fd, &msgh, 0);
    if (len < 0) die("recvmsg");

    // We check that the received auxiliary data is of the correct
    // type and that contains exactly one file descriptor.
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msgh);
    if (   cmsg->cmsg_level != SOL_SOCKET
        || cmsg->cmsg_type  != SCM_RIGHTS
        || cmsg->cmsg_len   != CMSG_LEN(sizeof(*fd)))
        die("recvmsg/SOL_SOCKET");

    // Extract the file descriptor and store it to the user provided
    // location.
    int *cmsg_fd = (int*)CMSG_DATA(cmsg);
    *fd = *cmsg_fd;

    // Length of message in buf. len <= bufsize
    return len;
}

int main(int argc, char *argv[]) {
    // Create an connection-oriented AF_UNIX socket that preserves
    // message boundaries.
    int sock_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (sock_fd < 0) die("socket");

    // Create an socket address that points to the UNIX socket file
    char *sock_name = "./socket";
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, sock_name, sizeof(addr.sun_path)-1);

    // Connect to that address on our socket
    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1)
        die("connect");

    // Receive message and file descriptor from the server
    int stdout_fd;
    char buf[4096];
    int len = recvfd(sock_fd, buf, sizeof(buf)-1, &stdout_fd);

    // Truncate the message until the first null byte and print out some information about the descriptor
    printf("Received message: `%s'", buf);

    // Resolve the link /proc/self/fd/<NUMBER> to give the user an idea what we have received
    len = snprintf(buf, sizeof(buf), "/proc/self/fd/%d", stdout_fd);
    char *linkname = buf + len + 1; 
    if (readlink(buf, linkname, sizeof(buf) - (linkname - buf)) < 0)
        die("readlink");
    printf(" with fd=%d: %s -> %s\n", stdout_fd, buf, linkname);

    // In an endless loop, we copy data from our stdin to the received file descriptor.
    while (true) {
        // We read a buffer full of data
        size_t len = read(STDIN_FILENO, buf, sizeof(buf));
        if (len < 0) die("read");
        if (len == 0) break;

        // As writes can be short, we issue writes until our buffer
        // becomes empty.
        int to_write = len;
        char *ptr = buf;
        do {
            int written = write(stdout_fd, ptr, to_write);
            to_write -= written;
            ptr   += len;
        } while (to_write > 0);
    }
}
