#define _GNU_SOURCE
#include <stdarg.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/audit.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>
#include <string.h>
#include <stdbool.h>

#define die(msg) do { perror(msg); exit(EXIT_FAILURE); } while(0)

// For seccomp(2) and close_range(2), we require own system call
// wrappers as glibc does not provide them.

// close_range(2) - close all file descriptors between minfd and maxfd
#ifndef __NR_close_range
#define __NR_close_range 436
#endif
static int sys_close_range(unsigned int minfd, unsigned int maxfd, int flags) {
    return syscall(__NR_close_range, minfd, maxfd, 0);
}
// seccomp(2) - manipulate the seccomp filter of the calling process
static int sys_seccomp(unsigned int operation, unsigned int flags, void* args) {
    return syscall(__NR_seccomp, operation, flags, args);
}

// For call_secure, a process consists of a pid (as we spawn
// the function in another process and pipe connection.
typedef struct {
    pid_t pid; // pid of the child process
    int pipe;  // read end of a pipe that we share with the child process
} secure_func_t ;

// Spawn a function within a seccomp-restricted child process
secure_func_t spawn_secure(void (*func)(void*, int), void* arg) {
    // Create the child pipe.
    // pipe[1] - the write end
    // pipe[0] - the read end
    int    pipe[2];
    if (pipe2(pipe,  O_CLOEXEC))
        die("pipe2");

    pid_t pid = fork();
    if (pid < 0) die("fork");
    if (pid > 0) { // Parent
        // Close the write end of the child pipe
        close(pipe[1]);

        // Create a handle for the called function
        secure_func_t ret = {.pid = pid, .pipe = pipe[0] };
        return ret;
    }

    ////////////////////////////////////////////////////////////////
    // Child after here

    // dup the pipe write end to the file descriptor 0
    if (dup2(pipe[1], 0) < 0)
        die("dup2");

    // Close all file descriptors above 1 (requires Linux 5.9)
    if (sys_close_range(1, ~0U, 0) == -1) {
        die("close_range");
    }

    // Enter the strict seccomp mode. From here on, only read(2),
    // write(2), _exit(2), and sigreturn(2) are allowed.
    if (sys_seccomp(SECCOMP_SET_MODE_STRICT, 0, NULL) < 0)
        die("seccomp");

    // Execute the given function with the given argument and an
    // output descriptor of 0 (see dup2 above)
    func(arg, 0);

    // We use the bare exit system call here to kill the child.
    // Reason for this is, that the glibc calls exit_group(2),
    // when we call _exit().
    syscall(__NR_exit, 0);

    // We place this unreachable here to avoid a compiler warning.
    // This builtin does _not_ generate any code, but it only
    // signals to the compiler, that we will never come here.
    __builtin_unreachable();
}

// Complete a previously spawned function and read at most bulen bytes
// into buf. The function returns -1 on error or the number of
// actually read bytes.
int complete_secure(secure_func_t f, char *buf, size_t buflen) {
    // Read from the child pipe
    int len = read(f.pipe, buf, buflen);

    // Wait for the process to exit (or to get killed by the seccomp filter)
    int wstatus;
    if (waitpid(f.pid, &wstatus, 0) < 0)
        return -1;

    // Only if the process exited normally and with exit state 0, the
    // function might have completed correctly
    if (WIFEXITED(wstatus) && (WEXITSTATUS(wstatus) == 0)) {
        return len;
    }
    return -1;
}

// Test function that is valid
void ok(void *arg, int fd) {
    write(fd, "Hallo", 5);
}

// test function that is quite bad
void fail(void *arg, int fd) {
    int fd2 = open("/etc/passwd", O_RDWR);
    close(fd2);
}

int main(int argc, char*argv[]) {
    char buf[128];
    int len;
    secure_func_t p1 = spawn_secure(ok, NULL);
    if ((len = complete_secure(p1, buf, sizeof(buf))) >= 0) {
        buf[len] = 0;
        printf("ok: %s\n", buf);
    } else {
        printf("ok failed: %d\n", len);
    }

    secure_func_t p2 = spawn_secure(fail, NULL);
    if ((len = complete_secure(p2, buf, sizeof(buf))) >= 0) {
        buf[len] = 0;
        printf("fail: %s\n", buf);
    } else {
        printf("fail failed: %d\n", len);
    }

}

