#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <linux/connector.h>
#include <linux/netlink.h>
#include <linux/cn_proc.h>

#define die(msg) do { perror(msg); exit(EXIT_FAILURE); } while(0)

// For a given PID, get the path of the executable
static char *execname(pid_t pid, char *buf, size_t bufsize) {
    char path[512];
    snprintf(path, sizeof(path)-1, "/proc/%d/exe", pid);

    // Read the linkname from the /proc file system
    int len = readlink(path, buf, bufsize-1);

    // Perhaps, the process has already exited and then the path in
    // proc no longer exists. Therefore, do not fail eagerly here.
    if (len < 0) return NULL;

    // Null-terminate the path and return it
    buf[len] = 0;
    return buf;
}

// For using cn_proc, we have to create an NETLINK_CONNECTOR socket
// and "connect" it. Please note that these sockets are not
// connect(2)'ed, but we bind it to the correct sockaddr_nl address.
int cn_proc_connect() {
    // Create a netlink socket. Datagram-oriented is just fine here.
    int sock = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_CONNECTOR);
    if (sock < 0) die("socket");

    // We bind the socket to the given CN (connector netlink) address.
    struct sockaddr_nl addr;
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = CN_IDX_PROC; // The cn_proc multicast group

    // nl_pid is special: It is the unique identifier of our netlink
    // socket (like a UDP port). By setting it to 0, we let the
    // kernel assign an unique address. See netlink(7), nl_link
    addr.nl_pid  = 0;

    // Bind the socket! Addr: AF_NETLINK, mcast-group CN_IDX_PROC, our unique id: by kernel
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        die("bind");

    return sock;
}

// Without configuration, the Linux kernel does not create
// fork/exec/exit messages on the CN_IDX_PROC multicast group.
// Therefore, we have to send a message to the kernel to enable those
// messages. This function enables/disables that multicast stream.
//
// Internally, the kernel keeps track (with a counter) how often
// PROC_CN_MCAST_{LISTEN|IGNORE} was called. Interestingly, if your
// process does not disable this mcast again, subsequent binds will
// directly produce proc_event messages.
void cn_proc_configure(int sock_fd, bool enable) {
    // For enabling, the CN_PROC mcast, we have to send an message
    // that consists of three parts:
    // +-------------------------------------------------------+
    // |  struct    | Padd  | struct       |  enum             |
    // |  nlmsghdr  | -ing  | cn_msg       |  proc_cn_mcast_op |
    // +-------------------------------------------------------+
    // 
    // Thereby, the nlmsghdr and the cn_msg have length fields of the
    // following data. Think of this as encapsulated protocols. We
    // send cn_mcast_operation as a connector message over an netlink
    // transport.
    // The padding is: NLMSG_LENGTH(0) - sizeof(struct nlmsghdr)
    
    // The mcast operation.
    enum proc_cn_mcast_op mcast =
        enable ? PROC_CN_MCAST_LISTEN : PROC_CN_MCAST_IGNORE;

    // The connector message header indicates that this message is
    // directed at the cn_proc component
    struct cn_msg cn_hdr = {
        .id = {.idx = CN_IDX_PROC, .val = CN_VAL_PROC },
        .seq = 0, .ack = 0,
        .len = sizeof(mcast), // Length of the following payload
    };

    // The netlink header
    struct nlmsghdr nl_hdr = {
        // Length of the following payload
        .nlmsg_len =  NLMSG_LENGTH(sizeof(cn_hdr) + sizeof(mcast)),
        // Last (and first) part of this message
        .nlmsg_type  = NLMSG_DONE, 
    };

    // On some architectures, a few padding bytes are required between
    // the netlink header and the netlink payload. On amd64, you get
    // away with forgetting this.
    char padding[NLMSG_LENGTH(0) - sizeof(struct nlmsghdr)];

    // A scattered iovec that assembles the netlink message
    struct iovec vec[] = {
        { .iov_base = &nl_hdr,  .iov_len=sizeof(nl_hdr)  }, // 1.
        { .iov_base = &padding, .iov_len=sizeof(padding) }, // 2.
        { .iov_base = &cn_hdr,  .iov_len=sizeof(cn_hdr)  }, // 3.
        { .iov_base = &mcast,   .iov_len=sizeof(mcast)   }  // 4.
    };

    // Send it to the kernel
    if (writev(sock_fd, vec, sizeof(vec)/sizeof(*vec)) != nl_hdr.nlmsg_len)
        die("sendmsg");

}

// This function handles a single cn_proc event and prints it to the terminal
void cn_proc_handle(struct proc_event *ev) {
    // See /usr/include/linux/cn_proc.h for details

    char buf[256]; // Buffer for execname

    switch(ev->what){
    case PROC_EVENT_FORK:
        printf("fork(): %20s (%d, %d) -> (%d, %d)\n",
               execname(ev->event_data.fork.parent_tgid, buf, sizeof(buf)),
               ev->event_data.fork.parent_tgid,
               ev->event_data.fork.parent_pid,
               ev->event_data.fork.child_tgid,
               ev->event_data.fork.child_pid);
        break;
    case PROC_EVENT_EXEC:
        printf("exec(): %20s (%d, %d)\n",
               execname(ev->event_data.exec.process_tgid, buf, sizeof(buf)),
               ev->event_data.exec.process_tgid,
               ev->event_data.exec.process_pid);
        break;
    case PROC_EVENT_EXIT:
        printf("exit(): %20s (%d, %d) -> rc=%d\n",
               "",
               ev->event_data.exit.process_tgid,
               ev->event_data.exit.process_pid,
               ev->event_data.exit.exit_code);
        break;
    default:
        break;
    }
}


int cn_proc_fd;

void cn_proc_atexit() {
    cn_proc_configure(cn_proc_fd, false);
}

int main(int argc, char **argv) {
    // Unfortunately, the cn_proc interface is only available if you
    // have CAP_NET_ADMIN. As the most simple overapproximation of
    // this, we require the tool to be run as root.
    if (getuid() != 0) {
        fprintf(stderr, "must be run as root\n");
        return -1;
    }

    // Create a cn_proc socket and enable the mcast group
    cn_proc_fd = cn_proc_connect();
    atexit(cn_proc_atexit);
    cn_proc_configure(cn_proc_fd, true);

    // Receive events from the kernel
    while (true) {
        // Get multiple netlink messages from the kernel
        char buf[4096];
        int len = recv(cn_proc_fd, buf, sizeof(buf), 0);
        if (len < 0) die("recv");

        // Iterate over the netlink messages in the buffer. For this,
        // we use the helper macros from netlink(3) as netlink message
        // are weirdly (with padding) within the receive buffer
        for (/* init */ struct nlmsghdr *nlh = (struct nlmsghdr *) buf;
             /* cond */ NLMSG_OK (nlh, len); 
             /* next */ nlh = NLMSG_NEXT (nlh, len)) {

            // Only complete messages (ignore NOOP, ERROR, OVERRUN, whatever)
            if (nlh->nlmsg_type != NLMSG_DONE)
                continue;

            // All messages should come from cn_proc, but you never
            // can be sure.
            struct cn_msg *cn_msg = NLMSG_DATA(nlh);
            if ((cn_msg->id.idx != CN_IDX_PROC)
                || (cn_msg->id.val != CN_VAL_PROC))
                continue;

            // cn_msg->data contains a single proc_event
            cn_proc_handle((struct proc_event*)cn_msg->data);
        }
    }
    return 0;
}
