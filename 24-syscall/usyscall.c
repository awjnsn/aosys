#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <sys/uio.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <stdbool.h>
#include <dlfcn.h>
#include <ctype.h>
#include <stdint.h>



#define die(msg) do { perror(msg); exit(EXIT_FAILURE); } while(0)

// A ROT13 implementation
#define rot13(c) (isalpha(c)?(c&96)+1+(c-(c&96)+12)%26:c)

// Function prototype
void usyscall_init(void *offset, ssize_t length);
void usyscall_signal(int signum, siginfo_t *info, void *context);
void usyscall_enable(bool enable);

// This flag indicates to the kernel whether system calls are
// currently blocked or allowed. We start with allowed.
volatile char usyscall_flag = SYSCALL_DISPATCH_FILTER_ALLOW;

// Enable user space system call dispatching, but exclude the region
// from [offset, offset+length].
void usyscall_init(void *offset, ssize_t length) {
    // Install our own system call handler and request the SIGINFO
    // variant, which we need to get all necessary information to
    // reinject the system call.
    struct sigaction sa = {0};
    sigemptyset(&sa.sa_mask);
    sa.sa_flags     = SA_SIGINFO;
    sa.sa_sigaction = &usyscall_signal;

    if (sigaction(SIGSYS, &sa, NULL) != 0)
        die("sigaction");

    // Just enable the user space system call dispatcher.
    if (prctl(PR_SET_SYSCALL_USER_DISPATCH,
              PR_SYS_DISPATCH_ON,
              offset, length, &usyscall_flag) < 0) {
        fprintf(stderr, "kernel too old? (requires at least 5.11)\n");
        die("prctl");
    }
}

// Just a wrapper function to enable the usyscall mechanism
void usyscall_enable(bool enable) {
    usyscall_flag = enable
        ? SYSCALL_DISPATCH_FILTER_BLOCK
        : SYSCALL_DISPATCH_FILTER_ALLOW;
}

void usyscall_signal(int signum, siginfo_t *info, void *context) {
    usyscall_enable(false);

    ucontext_t *ctx = (ucontext_t *)context;
    uint64_t args[6] = {
        ctx->uc_mcontext.gregs[REG_RDI],
        ctx->uc_mcontext.gregs[REG_RSI],
        ctx->uc_mcontext.gregs[REG_RDX],
        ctx->uc_mcontext.gregs[REG_R10],
        ctx->uc_mcontext.gregs[REG_R9],
        ctx->uc_mcontext.gregs[REG_R8]
    }; (void) args;

    // We have the problem that returning from a signal requires a
    // system call (rt_sigreturn). However, we want to return with an
    // enabled usyscall dispatcher. Therefore, we have to allow
    // syscalls in a small area in the glibc.
    static bool return_rt_allowed = false;
    if (!return_rt_allowed) { // Executed exactly once
        return_rt_allowed = true;

        // We will return to a stub function in the glibc that issues
        // the sigreturn system call. We use a compiler instrinstic to get our return address
        void *__return_rt = __builtin_extract_return_addr(__builtin_return_address (0));

        // Reinitialize the usyscall mechanism
        usyscall_init(__return_rt, 20);
    }

    // Special treatment for some system calls
	if (info->si_syscall == __NR_write) {
        if (args[0] == STDOUT_FILENO) {
            // For write to stdout, we use rot13 to encrypt the
            // output. We copy the source buffer here (and issue
            // multiple writes in case) to avoid modifying the source buffer
            char *orig   = (char*) args[1];
            size_t count = args[2];
            char buf[64];
            for (unsigned i = 0; i < count; i++) {
                buf[i % sizeof(buf)] = rot13(orig[i]);
                if (i == (sizeof(buf) - 1))
                    write(1, buf, 64);
            }
            write(1, buf, count % sizeof(buf));
        }
	} else if (info->si_syscall == 512) {
        // We also introduce a new system call with the number 512
        printf("MySyscall: 0x%lx\n", args[0]);
    } else {
        // For all other system calls, we just execute the actual system call.
        int rax = syscall(info->si_syscall, args[0], args[1], args[2], args[3], args[4], args[5]);
        ctx->uc_mcontext.gregs[REG_RAX] = rax;
    }

    // Block system calls again
    usyscall_enable(true);


    // A return calls the rt_sigreturn system call. This has to be
    // allowed here as the (offset+length) of prctl. length=20 bytes
    // is enough for glibc.
}

int main(int argc, char **argv) {
    usyscall_init(NULL, 0);

    write(1, "Hallo Welt\n", 12);

    usyscall_enable(true);

    write(1, "Hallo Welt\n", 12);

    syscall(512, 0xdeadbeef);

    usyscall_enable(false);

    write(1, "Hallo Welt\n", 12);

    return 0;
}
