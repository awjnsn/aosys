#define _GNU_SOURCE
#include <stdio.h>
#include <spawn.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <stdlib.h>

#define die(msg) do { perror(msg); exit(EXIT_FAILURE); } while(0)

// This function starts a program within a PTY by setting the process'
// stdin, stdout, and stderr to the same pseudo terminal file
// descriptor. Please note that pty_fd is the open file descriptor of
// the child (e.g. /dev/pts/7)
static pid_t exec_in_pty(char *argv[], int pty_fd) {
    // We use posix_spawnp(3) to perform the process creation. For the
    // redirection of std{in,out,err}, we use a file action which
    // performs a dup(2) *after* the fork. For details, please look at
    // the man page of posix_spawnp.
    //
    //     dup2(pty_fd, STDIN_FILENO);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pty_fd, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&fa, pty_fd, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, pty_fd, STDERR_FILENO);


    // In order to allow shells within our pseudo terminal, we have to
    // spawn the new process as a new session leader. While this is an
    // interesting topic on its own, we won't cover it today.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);

    // "magic variable": array of pointers to environment variantes.
    // This symbol is described in environ(2)
    extern char **environ;

    // We spawn the process with posix_spawnp(3). Please note the
    // spawnp searches the PATH variable and allows us to write:
    //
    // ./scribble OUT IN bash
    //
    // instead of
    //
    // ./scribble OUT IN /bin/bash
    pid_t pid;
    if (posix_spawnp(&pid, argv[0], &fa, &attr, argv,  environ) != 0)
        die("posix_spawn");

    // Cleanup everything
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);

    return pid;
}

// For new interactive processes to work correctly within our pty, we
// have to set a few options. As this is also quite cumbersome to find
// out, we will only look at the result here.
static struct termios orig_termios; // <- We store the old configuration

// Restore the original termios setting
void restore_terminal() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

// Configure our terminal
void configure_terminal() {
    // Credits go to:
    // https://viewsourcecode.org/snaptoken/kilo/02.enteringRawMode.html
    // see also termios(3)

    //  Get the original termios setting
    tcgetattr(STDIN_FILENO, &orig_termios);
    // Register an atexit(3) handler to surely restore it when the process exits
    atexit(restore_terminal);

    // We delete the flags and configure the terminal
    // ~ECHO:   Disable direct echoing of input by the terminal 
    // ~ICANON: Disable canonical mode (line buffered, line editing, etc..)
    // ~ISIG:   Disable C-c to send a signal
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG );
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

struct copy_thread_arg {
    int src_fd;
    int dst_fd;
    int dump_fd;
};

void* copy_thread(void* data) {
    struct copy_thread_arg *arg = data;

    char buf[1024];
    while (true) {
        int len = read(arg->src_fd, buf, sizeof(buf));
        if (len < 0)
            die("read");
        if (len == 0) break;

        for (int bufpos = 0; bufpos < len; ) {
            int wlen = write(arg->dst_fd, buf+bufpos, len - bufpos);
            if (wlen < 0)
                die("write");
            bufpos += wlen;
        }

        for (int bufpos = 0; bufpos < len; ) {
            int wlen = write(arg->dump_fd, buf+bufpos, len - bufpos);
            if (wlen < 0)
                die("write");
            bufpos += wlen;
        }
    }
    return NULL;
}


int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s OUT IN CMD [ARG ...]", argv[0]);
        return -1;
    }
    char *OUT    = argv[1];
    char *IN     = argv[2];
    char **CMD   = &argv[3];

    // We open the dump file for the terminal output OUT and
    // for the terminal input IN. We open it read-writable (O_RDWR),
    // create it in case (O_CREAT), truncate it to zero bytes
    // (O_TRUNC), and instruct the kernel to close the file descriptor
    // on exec (O_CLOEXEC). With O_CLOEXEC, the descriptor is not available in our child.
    int out_fd = open(OUT, O_RDWR|O_CREAT|O_TRUNC|O_CLOEXEC, 0600);
    if (out_fd < 0)
        die("open/dump");

    int in_fd = open(IN, O_RDWR|O_CREAT|O_TRUNC|O_CLOEXEC, 0600);
    if (in_fd < 0)
        die("open/dump");

    // We allocate a new pseudo terminal by opening the special device
    // file /dev/ptmx. Please see pty(7) for more details on this.
    // Special is the O_NOCTTY flag which disables the magic of
    // setting a control terminal.
    int primary_fd = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (primary_fd < 0) die("open/ptmux");

    // And now it goes wild! Like a pipe, a pty has two ends. The
    // primary and the child end (also referred to as secondary). But
    // unlike the pipe2(2) system call, we have to deduce the child
    // ptn (pseudo-terminal number) from the primary fd.

    // For the Unix-98 interface, this can simply be done with an
    // ioctl. With BSD pseudo-terminals it is not that easy.
    int ptn;
    if (ioctl(primary_fd, TIOCGPTN, &ptn) < 0)
        die("ioctl/TIOCGPTN");

    // This unlocks the secondary pseudo-terminal. In the libc, this is
    // implemented by unlockpt(3).
    int unlock = 0;
    if (ioctl(primary_fd, TIOCSPTLCK, &unlock) < 0)
        die("ioctl/TIOCSPTLOCK");


    // Having the pts, we can create a ptsname(3) filename and open
    // the child end of our pty. Somehow, with pipe2(2), this was much
    // easier up to this point.
    char ptsname[128];
    sprintf(ptsname, "/dev/pts/%d", ptn);
    int secondary_fd = open(ptsname, O_RDWR|O_NOCTTY|O_CLOEXEC);
    if (secondary_fd < 0)
        die("open/pts");

    printf("primary=%d, pts=%s, child=%d\n", primary_fd, ptsname, secondary_fd);

    // We configure our terminal to pass through some keys and to not
    // echo everything twice.
    configure_terminal();

    // Spawn the process into our newly created pty.
    pid_t pid = exec_in_pty(CMD, secondary_fd);
    printf("child pid=%d\n", pid);

    // We create two threads that copy data from
    // 1. From STDIN      to the primary fd _and_ the IN file descriptor
    // 2. From primary_fd  to our STDOUT    _and_ the OUT file descriptor
    struct copy_thread_arg args[] = {
        { .src_fd = STDIN_FILENO, .dst_fd = primary_fd,     .dump_fd = in_fd},
        { .src_fd = primary_fd,    .dst_fd = STDOUT_FILENO, .dump_fd = out_fd},
    };

    // We use two threads to perform this task to avoid all problems
    // with blocking. Thereby, we avoid the need to coordinate
    // everything with epoll(2), which you are probably tired of anyway.
    pthread_t threads[2];
    for (unsigned i = 0; i < 2; i++) {
        int rc = pthread_create(&threads[i], NULL, copy_thread, &args[i]);
        if (rc < 0) die("pthread_create");
    }

    // In the main thread, we simply wait with waitpid(2) for the
    // child process to exit.
    while (1) {
        int wstatus;
        if (waitpid(pid, &wstatus, 0) < 0)
            die("waitpid");
        if (WIFEXITED(wstatus)) {
            fprintf(stderr,"child process exited with: %d\n", WEXITSTATUS(wstatus));
            exit(WEXITSTATUS(wstatus));
        }
    }
}
