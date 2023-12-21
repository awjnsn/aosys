#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <limits.h>
#include <sys/un.h>
#include <sys/socket.h>

#define die(msg) do { perror(msg); exit(EXIT_FAILURE); } while(0)

// Send a file descriptor over an connected UNIX domain socket. The
// file descriptor is send as auxiliary data attached to a regular
// buffer. With Linux, we have to send at least one byte to transfer a
// file descriptor.
//
// sockfd:      connected UNIX domain socket
// buf, buflen: Message to send, arbitrary data
// fd:          file descriptor to transfer
void sendfd(int sockfd, void *buf, size_t buflen, int fd) {
    // We use sendmsg, which requires an iovec to describe the data
    // sent. We already know this structure from the writev exercise
    struct iovec data = { .iov_base = buf, .iov_len = buflen};

    // Ancillary data buffer. We wrap it into an anonymous union to
    // ensure correct alignment.
    union {
        // Buffer with enough space to hold a single descriptor
        char buf[CMSG_SPACE(sizeof(fd))];
        struct cmsghdr align;
    } auxdata;

    // We create a message header that points to our data buffer and
    // to our auxdata buffer.
    struct msghdr msgh = {
        // Normal data to send
        .msg_iov        = &data,
        .msg_iovlen     = 1,
        // Buffer for control data. See cmsg(3) for more details
        .msg_control    = auxdata.buf,
        .msg_controllen = sizeof(auxdata.buf),
    };

    // We prepare the auxiliary data
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msgh);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS; // See unix(7), SCM_RIGHTS
    cmsg->cmsg_len   = CMSG_LEN(sizeof(fd));

    // Set the file descriptor into the cmsg data
    int *cmsg_fd = (int*)CMSG_DATA(cmsg);
    *cmsg_fd = fd;

    // Send our prepared message with sendmsg(2)
    if (sendmsg(sockfd, &msgh, 0) == -1)
        die("sendmsg");
}

int main() {
    // Create a new UNIX domain socket. We use the SOCK_SEQPACKET
    // socket type as it is a connection oriented socket (others
    // connect to it), but it ensures message boundaries. Otherwise,
    // the other side has to read exactly as many bytes as we write in
    // order to get the auxiliary data correctly.
    int sock_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (sock_fd < 0) die("socket");

    // Bind the socket to a filename. We already have seen that in the
    // postbox exercise.
    char *sock_name = "./socket";
    struct sockaddr_un sockaddr = {.sun_family = AF_UNIX };
    strcpy(sockaddr.sun_path, sock_name);

    int rc = unlink(sock_name);
    if (rc < 0 && errno != ENOENT) die("unlink/socket");

    rc = bind(sock_fd, (struct sockaddr *) &sockaddr, sizeof(sockaddr));
    if (rc == -1) die("bind/socket");

    // As we are connection oriented, we listen on the socket.
    rc = listen(sock_fd, 10);
    if (rc < 0) die("listen/socket");

    printf("Please connect: ./client\n");

    while (true) {
        // We wait for a client and establish a connection.
        int client_fd = accept(sock_fd, NULL, NULL);
        if (client_fd < 0) die("accept");

        printf("Client on fd=%d. Sending STDOUT\n", client_fd);

        // Send our STDOUT file descriptor with an accompanying message
        sendfd(client_fd, "STDOUT", strlen("STDOUT"), STDOUT_FILENO);

        // Directly close the client fd again.
        close(client_fd);
    }
}

