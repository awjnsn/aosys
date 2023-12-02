#define _GNU_SOURCE
#include <sched.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>
#include <string.h>
#include <syscall.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

/* For our clone experiments, we are working on a very low level and
 * fiddle around with threading. This leads to a problem with the
 * libc, which must perform some user-space operations to setup a
 * new thread. For example, in a clone()ed thread, we cannot simply
 * use printf(). Therefore, we provide you with a simple function to
 * output a string and a number. The writing to stdout (fd=1) is done
 * with plain system calls.
 *
 * Example: syscall_write("foobar = ", 23);
 */
int syscall_write(char *msg, int number) {
    write(1, msg, strlen(msg));
    if (number != 0) {
        char buffer[sizeof(number) * 3];
        char *p = &buffer[sizeof(number) * 3];
        int len = 1;
        *(--p) = '\n';
        if (number < 0) {
            write(1, "-", 1);
            number *= -1;
        }
        while (number > 0) {
            *(--p) =  (number % 10) + '0';
            number /= 10;
            len ++;
        }
        write(1, p, len);
    } else {
        write(1, "0\n", 2);
    }

    return 0;
}

// For the new task, we always require an stack area. To make our life
// easier, we just statically allocate an global variable of PAGE_SIZE.
char stack[4096];

// To demonstrate whether child and parent are within the same
// namespace, we count up a global variable. If they are within the
// same address space, we will see modification to this counter in
// both namespaces
volatile int counter = 0;

int child_entry(void* arg) {
    // We just give a little bit of information to the user.
    syscall_write(": Hello from child_entry", 0);
    syscall_write(": getppid() = ", getppid()); // What is our parent PID
    syscall_write(": getpid()  = ", getpid());  // What is our thread group/process id
    syscall_write(": gettid()  = ", gettid());  // The ID of this thread!
    syscall_write(": getuid()  = ", getuid());  // What is the user id of this thread.

    if (arg != NULL) {
        // We got a uid_map. So we install it
        int fd = open("/proc/self/uid_map", O_RDWR);
        char *uid_map = (char*) arg;
        write(fd, uid_map, strlen(uid_map));
        close(fd);

        // With the uid_map in place, we have become 'root' in our
        // namespace, as the PID from outside maps to the UID=0 within
        // the namespace. So, we've become root... but not for real!
        syscall_write(": getuid()  = ", getuid());
        syscall_write(": setuid() = ", setuid(0));
    }

    // We increment the global counter in one second intervals. If we
    // are in our own address space, this will have no influence on
    // the parent!
    while (counter < 4) {
        counter++;
        sleep(1);
    }

    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("usage: %s MODE\n", argv[0]);
        printf("MODE:\n");
        printf("  - fork    -- emulate fork with clone\n");
        printf("  - chimera -- create process/thread chimera\n");
        printf("  - thread  -- create a new thread in a process\n");
        printf("  - user    -- create a new process and alter its UID namespace\n");
        return -1;
    }

    syscall_write("> Hello from main!", 0);
    syscall_write("> getppid() = ", getppid());
    syscall_write("> getpid()  = ", getpid());
    syscall_write("> gettid()  = ", gettid());
    syscall_write("> getuid()  = ", getuid());

    int flags = 0;
    void *arg = NULL;
    if (!strcmp(argv[1], "fork")) {
        // For a fork, we usually want to recieve a SIGCHLD signal if
        // the child's process group terminates.
        flags = SIGCHLD;
    } else if (!strcmp(argv[1], "chimera")) {
        // For the chimera, we _only_ share the virtual memory with
        // the child process. Thereby, a new process but with the same
        // address space is created.
        flags = SIGCHLD | CLONE_VM;
    } else if (!strcmp(argv[1], "thread")) {
        // For a real thread, we also put the child in the same thread
        // group. Please note that Linux requires us to also share the
        // signal handler table, if two threads want to live in the
        // same thread group.
        flags = CLONE_VM | CLONE_THREAD | CLONE_SIGHAND;
    } else if (!strcmp(argv[1], "user")) {
        // For the UID namespace, we just do a fork, but request a new
        // user namespace for it.
        flags = SIGCHLD | CLONE_NEWUSER;

        // In order to actually use this new namespace, we have to
        // install a simple uid_map. Tomake our life easy, we just
        // create the uid_map within the parent, pass it as argument
        // to clone and write it there to the /proc/self/uid_map
        // pseudo file.
        arg = malloc(100);
        snprintf((char*)arg, 99, "0 %d 1\n", getuid());
    } else {
        printf("Invalid clone() mode: %s\n", argv[1]);
        return -1;
    }

    // For the clone system call, we specify the start address, and
    // pass a pointer to the _top_ of stack.
    pid_t pid = clone(child_entry, &stack[sizeof(stack)-1], flags, arg);
    if (pid == -1) {
        perror("clone");
        return -1;
    }
    syscall_write("> clone() returned ", pid);

    syscall_write("\n!!!!! Press C-c to terminate. !!!!!", 0);
    while(counter < 4) {
        syscall_write("counter = ", counter);
        sleep(1);
    }

    return 0;
}
