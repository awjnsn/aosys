#define _GNU_SOURCE
#include <stdio.h>
#include <spawn.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <assert.h>
#include <limits.h>


#define die(msg) do { perror(msg); exit(EXIT_FAILURE); } while(0)

/* For each filter process, we will generate a proc object */
struct proc {
    char    *cmd;   // command line
    pid_t   pid;    // process id of running process. 0 if exited
    int     stdin;  // stdin file descriptor of process (pipe)
    int     stdout; // stdout file descriptor of process
};

static int nprocs;         // Number of started filter processes
static struct proc *procs; // Dynamically-allocated array of procs

////////////////////////////////////////////////////////////////
// HINT: You have already seen this in the in the select exercise
////////////////////////////////////////////////////////////////

// This function starts the filter (proc->cmd) as a new child process
// and connects its stdin and stdout via pipes (proc->{stdin,stdout})
// to the parent process.
//
// We also start the process wrapped by stdbuf(1) to force
// line-buffered stdio for a more interactive experience on the terminal
static int start_proc(struct proc *proc) {
    // We build an array for execv that uses the shell to execute the
    // given command.
    char *argv[] = {"sh", "-c", proc->cmd, 0 };

    // We create two pipe pairs, where [0] is the reading end
    // and [1] the writing end of the pair. We also set the O_CLOEXEC
    // flag to close both descriptors when the child process is exec'ed.
    int stdin[2], stdout[2];
	if (pipe2(stdin,  O_CLOEXEC)) return -1;
    if (pipe2(stdout, O_CLOEXEC)) return -1;

    // For starting the filter, we use posix_spawn, which gives us an
    // interface around fork+exec to perform standard process
    // spawning. We use a filter action to copy our pipe descriptors to
    // the stdin (0) and stdout (1) handles within the child.
    // Internally, posix_spawn will do a dup2(2). For example,
    //
    //     dup2(stdin[0], STDIN_FILENO);
    posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, stdin[0],  STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&fa, stdout[1], STDOUT_FILENO);

    // "magic variable": array of pointers to environment variantes.
    // This symbol is described in environ(2)
    extern char **environ;

    // We spawn the filter process.
    int e;
    if (!(e = posix_spawn(&proc->pid, "/bin/sh", &fa, 0, argv,  environ))) {
        // On success, we free the allocated memory.
        posix_spawn_file_actions_destroy(&fa);

        // We are within the parent process. Therefore, we copy our
        // pipe ends to the proc object and close the ends that are
        // also used within the child (to save file descriptors)

        // stdin of filter
        proc->stdin = stdin[1]; // write end
        close(stdin[0]);        // read end

        // stdout of filter
        proc->stdout = stdout[0]; // read end
        close(stdout[1]);         // write end

        return 0;
	} else {
        // posix_spawn failed.
        errno = e;
        return -1;
    }
}



// Adds a file descriptor to an open epoll instance's list of
// interesting file descriptors.
//// events -  For which events are we waiting (usually EPOLLIN)
//// data   -  The kernel returns this data when an event occurs 
void epoll_add(int epoll_fd, int fd, int events, uint64_t data) {
    struct epoll_event ev;
    ev.events   = events;
    ev.data.u64 = data;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        die("epoll_ctl: activate");
    }
}

// Remove a file descriptor from the interest list.
void epoll_del(int epoll_fd, int fd) {
    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL) == -1) {
        die("epoll_ctl: reset");
    }
}

// This function copies (some) bytes from in_fd to out_fd, as fast as
// possible. For this we have a fast path with splice(2) and a slow
// path with read(2)/write(2)
int copy_splice(int in_fd, int out_fd) {
    // We use a static buffer for the slow path
    static char buf[4096];

    // First, we try our fast path. We instruct splice to move as many
    // data as possible (up to INT_MAX) between the descriptors.
    // However, the splice should not block.
    int len = splice(in_fd, 0, out_fd, 0, INT_MAX, SPLICE_F_NONBLOCK);
    if (len >= 0) return len;

    // Splice would have blocked, we will try again later on
    if (errno == EAGAIN)
        return 0;

    // Splice fails with EINVAL if source or destination fd are not
    // spliceable (e.g., the terminal), we fall back to our slow path
    // and do doing regular read/write I/O.

    if (errno != EINVAL) die("splice");

    // We read a buffer full of data
    len = read(in_fd, buf, 4096);
    if (len < 0) die("read");

    // As writes can be short, we issue writes until our buffer
    // becomes empty.
    int to_write = len;
    char *ptr = buf;
    do {
        int written = write(out_fd, ptr, to_write);
        to_write -= written;
        ptr   += len;
    } while (to_write > 0);

    return len;
}

// This function prints an array of uint64_t (elements) as line with
// throughput measures. The function throttles its output to one line
// per second.

// Example Output:
//  2860.20MiB/s 2860.26MiB/s 2860.23MiB/s 2860.25MiB/s 2860.29MiB/s
void print_throughput(uint64_t *bytes, int elements) {
    static struct timespec last = { 0 };

    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) < 0)
        die("clock_gettime");

    if (now.tv_sec > last.tv_sec && last.tv_sec > 0) {
        double delta = now.tv_sec - last.tv_sec;
        delta += (now.tv_nsec - last.tv_nsec) / 1e9;
        for (int i = 0; i < elements; i++) {
            fprintf(stderr, " %.2fMiB/s", bytes[i]/delta/1024/1024);
        }
        fprintf(stderr, "\n");
        memset(bytes, 0, elements * sizeof(uint64_t));
        last = now;
    } else if (last.tv_sec == 0) {
        last = now;
    }
}

int main(int argc, char *argv[]) {
    if (argc <= 1) {
        fprintf(stderr, "usage: %s [CMD-1]", argv[0]);
        return -1;
    }
    // We allocate an array of proc objects
    nprocs = argc - 1;
    procs   = malloc(nprocs * sizeof(struct proc));
    if (!procs) die("malloc");

    // Initialize proc objects and start the filter
    for (int i = 0; i < nprocs; i++) {
        procs[i].cmd  = argv[i+1];
        int rc = start_proc(&procs[i]);
        if (rc < 0) die("start_filter");

        fprintf(stderr, "[%s] Started filter as pid %d\n", procs[i].cmd, procs[i].pid);
    }

    // We create an epoll instance.
    int epoll_fd = epoll_create1(0);
	if (epoll_fd == -1)
        die("epoll_create");

    // For each fd->fd connection we have a read_fd and an write_fd.
    // On a higher level, this program connects file descriptors as
    // pairs like this:
    //
    //    read_fds[i] ----> write_fds[i]
    int pairs = 1 + nprocs;
    int read_fds[pairs];
    int write_fds[pairs];

    // Arrange descriptors in read_fds/write_fds arrays as pairs
    read_fds[0] = STDIN_FILENO;
    for (int i = 0; i < nprocs; i++) {
        write_fds[i]  = procs[i].stdin;
        read_fds[i+1] = procs[i].stdout;
    }
    write_fds[nprocs] = STDOUT_FILENO;

    // We now setup the epoll device. We listen for EPOLLIN on the
    // read_fds and set the pair index as event data.
    for (int i = 0; i < 1 + nprocs; i++)
        epoll_add(epoll_fd, read_fds[i], EPOLLIN, i);

    // For the throughput measurements, we use this array to store how
    // many bytes we have transferred for a given pair. 
    uint64_t bytes[pairs];
    memset(bytes, 0, sizeof(bytes));


    // As long as we have active pairs, we continue to listen for
    // events on our epoll device.
    while (pairs > 0) {
        // We use epoll_wait(2) to wait for at least one event, but we
        // can receive up to ten events. We do not set a timeout but
        // wait forever if necessary.
        struct epoll_event event[10];
        int nfds = epoll_wait(epoll_fd, event, 10, -1);

        // Consume the events.
        for (int n = 0; n < nfds; n++) {
            // The kernel has given us our pair identifier. This is
            // the great benefit of the epoll interface over
            // select(2). With this user_data tunneled through the
            // kernel, we can directly identify the event in our
            // user space logic.
            uint64_t pair = event[n].data.u64;

            // If new data arrived, we shuffle data between the paired fds.
            if (event[n].events & EPOLLIN)  {
                bytes[pair] += copy_splice(read_fds[pair], write_fds[pair]);
            }

            // If the input was closed (the other end hung up), we
            // delete the event, close the file descriptors of the
            // pair and decrement the number of active pairs.
            if (event[n].events & EPOLLHUP)  {
                epoll_del(epoll_fd, read_fds[pair]);
                close(read_fds[pair]);
                close(write_fds[pair]);
                pairs --;
            }
        }

        // We try to dump the throughput information
        print_throughput(bytes, pairs);
    }
}
