#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/ptrace.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/eventfd.h>
#include <assert.h>
#include <string.h>

#define die(msg) do { perror(msg); exit(EXIT_FAILURE); } while(0)

// A table of system call names and argument counts
// This is only valid on x64 (AMD64)
#include "table.c"

// Print information about a system call (as strace also does it),
// which we got from PTRACE_GET_SYSCALL_INFO. This function also uses
// the system call table to print pretty syscall names.
void print_syscall(struct ptrace_syscall_info *info) {
    char buf[128];
    if (info->op == PTRACE_SYSCALL_INFO_ENTRY) {
        char *p = buf;
        int args = 6;
        if (info->entry.nr < (sizeof(names) / sizeof(*names))) {
            // Get the system call name
            p+= sprintf(p, "%s(", names[info->entry.nr].name);
            // Less than 6 arguments?
            if (names[info->entry.nr].argc < args)
                args = names[info->entry.nr].argc;
        } else {
            p+= sprintf(p, "syscall(%lld, ", info->entry.nr);
        }
        // Format the arguments
        for (unsigned i = 0; i < args; i++) {
            p += sprintf(p, "%lld", info->entry.args[i]);
            if ((i + 1) < args)
                p += sprintf(p, ", ");
        }
        p += sprintf(p, ")");
        fprintf(stderr, "%-70s", buf);
    } else if (info->op == PTRACE_SYSCALL_INFO_EXIT) {
        // The system call did complete
        if (info->exit.is_error) {
            fprintf(stderr, " = %lld (%s)\n",
                    info->exit.rval,
                    strerror(-1* info->exit.rval));
        } else {
            fprintf(stderr, " = %lld\n", info->exit.rval);
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc == 1) {
        fprintf(stderr, "usage: %s CMD [ARGS...]\n", argv[0]);
        return -1;
    }
    pid_t child = fork();
    if (child < 0)
        die("fork");

    // The child execs to the specified argv[1:]
    if(child == 0) {
        // The child allows its parent process to trace it.
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) < 0)
            die("ptrace/TRACEME");

        // This will yield to a SIGTRAP in the parent and stop the
        // process before the program is loaded.
        if (execvp(argv[1], &argv[1]) < 0)
            die("execvp");
    }

    ////////////////////////////////////////////////////////////////
    // The parent traces its child.

    // Wait for the execvp SIGTRAP
    int wstatus;
    if (waitpid(child, &wstatus, 0) < 0)
        die("waitpid");

    // Might already have died
    if (WIFEXITED(wstatus))
        exit(WEXITSTATUS(wstatus));

    // We also configure the tracing of the child.
    unsigned flags =
        // We want that the child is also killed if we die
          PTRACE_O_EXITKILL
        // We want to have the good and nice syscall tracing interface
        | PTRACE_O_TRACESYSGOOD;
    
    if (ptrace(PTRACE_SETOPTIONS, child, 0, flags) < 0)
        die("ptrace/SETOPTIONS");

    while(1) {
        // Continue the child to the next system call
        if (ptrace(PTRACE_SYSCALL, child, NULL, NULL) < 0)
            die("ptrace");

        // Wait for the child to reach the next trace point
        if (waitpid(child, &wstatus, 0) < 0)
            die("waitpid");

        // If it has exited, we exit with the same exit code
        if (WIFEXITED(wstatus))
            exit(WEXITSTATUS(wstatus));

        // We use PTRACE_GET_SYSCALL_INFO to extract information about
        // the system call. This is called twice:
        // - on syscall entry => (op == PTRACE_SYSCALL_INFO_ENTRY)
        // - on syscall exit  => (op == PTRACE_SYSCALL_INFO_EXIT)
        struct ptrace_syscall_info info;
        size_t size = sizeof(info);
        if (ptrace(PTRACE_GET_SYSCALL_INFO, child, &size, &info) < 0)
            die("ptrace/GET_SYSCALL_INFO");

        // Use the helper function to produce an output
        print_syscall(&info);
    }
}
