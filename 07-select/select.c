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
#include <sys/wait.h>

#define die(msg) do { perror(msg); exit(EXIT_FAILURE); } while(0)

/* For each filter process, we will generate a proc object */
struct proc {
    char *cmd;  // command line
    pid_t pid;  // process id of running process. 0 if exited
    int stdin;  // stdin file descriptor of process (pipe)
    int stdout; // stdout file descriptor of process

    // For the output, we save the last char that was printed by this
    // process. We use this to prefix all lines with a banner a la
    // "[CMD]".
    char last_char;
};

static int nprocs;         // Number of started filter processes
static struct proc *procs; // Dynamically-allocated array of procs

// This function starts the filter (proc->cmd) as a new child process
// and connects its stdin and stdout via pipes (proc->{stdin,stdout})
// to the parent process.
//
// We also start the process wrapped by stdbuf(1) to force
// line-buffered stdio for a more interactive experience on the terminal
static int start_proc(struct proc *proc) {
    // We build an array for execv that uses the shell to execute the
    // given command. Furthermore, we use the stdbuf tool to start the
    // filter with line-buffered output.

    //
    // HINT: We use the glibc asprintf(3), as I'm too lazy doing the
    //       strlen()+malloc()+snprintf() myself.
    //       Therefore we have to define _GNU_SOURCE at the top.
    char *stdbuf_cmd;
    asprintf(&stdbuf_cmd, "stdbuf -oL %s", proc->cmd);
    char *argv[] = {"sh", "-c", stdbuf_cmd, 0 };

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

    // "magic variable": array of pointers to environment variables.
    // This symbol is described in environ(2)
    extern char **environ;

    // We spawn the filter process.
    int e;
    if (!(e = posix_spawn(&proc->pid, "/bin/sh", &fa, 0, argv,  environ))) {
        // On success, we free the allocated memory.
        posix_spawn_file_actions_destroy(&fa);
        free(stdbuf_cmd);

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
        free(stdbuf_cmd);
        return -1;
    }
}



// For the input side, we use a separate thread that reads from our
// stdin and pushes the data to our filter processes. We do this in a
// separate thread to avoid deadlocks where we want to push data to a
// filter but the filter waits for us reading from its stdout.
// Alternatively, we could use select not only for waiting for
// read-ready file descriptors but also for write-ready ones.
void* stdin_thread(void* data) {
    // We need some buffer to handle the data. The size is actually arbitrary.
    int buflen = 4096;
    char *buf  = malloc(buflen);
    if (!buf) die("malloc");

    while (true) {
        // Read len bytes from _our_ stdin.
        int len = read(STDIN_FILENO, buf, buflen);

        // An error occurred or our stdin was closed. In this case,
        // we close the stdin of our filters and terminate the stdin
        // thread.
        if (len < 0 || len == 0) {
            // fprintf(stderr, "Stdin was closed %d\n", nprocs);
            for (int i = 0; i < nprocs; i++) {
                if (procs[i].pid != 0)
                    close(procs[i].stdin);
            }
            return NULL;
        }

        // We write the buffer to the stdin of all running processes.
        // We ignore all errors, because we like to live on the edge.
        for (int i = 0; i < nprocs; i++) {
            if (procs[i].pid != 0)
                write(procs[i].stdin, buf, len);
        }
    }
}

// When a process is ready and can provide us some data at his stdout,
// we call this function. Here we drain the stdout and print the
// result to our stdout. We print the output line wise and insert
// banners after each newline.
int drain_proc(struct proc *proc, char *buf, size_t buflen) {
    int len = read(proc->stdout, buf, buflen-1);
    if (len < 0) {
        die("write to filter");
    } else if (len == 0) { // EOF. process closed stdout.
        int state;
        waitpid(proc->pid, &state, 0);
        fprintf(stderr, "[%s] filter exited. exitcode=%d\n", proc->cmd, WEXITSTATUS(state));
        proc->pid = 0;
    }

    // Line-wise print with buffer. This is not meant to be effective
    for (int i = 0; i < len; i++) {
        if (proc->last_char == '\n')
            printf("[%s] ", proc->cmd);

        putchar(buf[i]);
        proc->last_char = buf[i];
    }
    return len;
}


int main(int argc, char *argv[]) {
    if (argc <= 1) {
        fprintf(stderr, "usage: %s [CMD-1] (<CMD-2> <CMD-3> ...)", argv[0]);
        return -1;
    }

    // We allocate an array of proc objects
    nprocs = argc - 1;
    procs   = malloc(nprocs * sizeof(struct proc));
    if (!procs) die("malloc");

    // Initialize proc objects and start the filter
    for (int i = 0; i < nprocs; i++) {
        procs[i].cmd  = argv[i+1];
        procs[i].last_char = '\n';
        int rc = start_proc(&procs[i]);
        if (rc < 0) die("start_filter");

        fprintf(stderr, "[%s] Started filter as pid %d\n", procs[i].cmd, procs[i].pid);
    }

    // We create an thread for handling of stdin.
    pthread_t handle;
    int rc = pthread_create(&handle, NULL, stdin_thread, NULL);
    if (rc < 0) die("pthread_create");

    // Allocate an buffer that we use for each filter
    const int MAX_LINE = 4096;
    char *buf = malloc(MAX_LINE);
    if (! buf) die("malloc");

    // In this loop, we use select(2) to wait for the proc[*].stdout
    // to become ready. If so, we read the data from that pipe and
    // print it to our stdout.
    while (true) {
        // First, we create a file descriptor set, which contains all
        // stdout descriptors of the running filter processes.
        // Thereby, we also have to calculate the maximum
        // file-descriptor number (nfds).
        int nfds = 0;
        fd_set readfds;
        FD_ZERO(&readfds);
        for (int i = 0; i < nprocs; i++) {
            if (procs[i].pid != 0) {
                FD_SET(procs[i].stdout, &readfds);
                if (nfds < procs[i].stdout)
                    nfds = procs[i].stdout;
            }
        }

        // No process is still alive, we can exit the program
        if (nfds == 0)
            break;

        // Use select(2) to wait for any proc[*].stdout to become ready
        int rc = select(nfds + 1, &readfds, NULL, NULL, NULL);
        if (rc == -1) die("select");


        // Determine which descriptor(s) became ready and drain the
        // process' stdout.
        for (int i = 0; i < nprocs; i++) {
            if (FD_ISSET(procs[i].stdout, &readfds)) {
                drain_proc(&procs[i], buf, MAX_LINE);
            }
        }
    }
}
