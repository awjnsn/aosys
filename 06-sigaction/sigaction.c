#undef _GNU_SOURCE
#define _GNU_SOURCE
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <stdio.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/ucontext.h>

int PAGE_SIZE;

extern int main(void);

/* See Day 2: clone.
 *
 * Example: syscall_write("foobar = ", 23);
 */
int syscall_write(char *msg, int64_t number, char base) {
    write(1, msg, strlen(msg));

    char buffer[sizeof(number) * 8];
    char *p = &buffer[sizeof(number) * 8];
    int len = 1;
    *(--p) = '\n';
    if (number < 0) {
        write(1, "-", 1);
        number *= -1;
    }
    do {
        *(--p) =  "0123456789abcdef"[number % base];
        number /= base;
        len ++;
    } while (number != 0);
    write(1, p, len);
    return 0;
}


/* We have three different fault handlers to make our program
 * nearly "immortal":
 *
 * 1. sa_sigint:  Is invoked on Control-C.
 * 2. sa_sigsegv: Handle segmentation faults
 * 3. sa_sigill:  Jump over illegal instructions
*/

volatile bool do_exit = false;
void  sa_sigint(int signum, siginfo_t *info, void *ucontext) {
    // We only set a global variable as a flag. Usually, we are only
    // allowed to call a very small subset of C library functions in
    // signal handlers. Therefore, flag-"signaling" is a usual
    // pattern in signal-handling code. In our case, we will invoke
    // pmap on our own process on Ctrl-C before actually exiting.
    do_exit = true;
}

void  sa_sigsegv(int signum, siginfo_t *info, void *context) {
    // From the siginfo_t structure, we get the faulting memory
    // address. This is the memory address that the user program tried
    // to access, but which yielded a SIGSEGV as there was no mapped
    // memory.
    syscall_write("sa_sigsegv: si_addr = 0x", (intptr_t) info->si_addr, 16);

    // Calculate the page address from the fault address
    // 0xdeadbeef -> 0xdeadb000
    uintptr_t addr = (uintptr_t)info->si_addr;
    addr = addr & (~ (PAGE_SIZE - 1));

    // MMAP one page of anonymous private memory to that location.
    // Afterwards, the program can continue its way to perdition.
    void *ret = mmap((void*)addr, PAGE_SIZE,
                     PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANONYMOUS,
                     -1, 0);
    if (ret == MAP_FAILED) {
        perror("mmap");
        exit(-1);
    }
    syscall_write("sa_sigsegv: mmap(PAGE_SIZE) -> 0x", addr, 16);
}


void sa_sigill(int signum, siginfo_t* info, void *context) {
    // The sa_sigill handler is a little bit more exotic as it uses
    // architecture specific macros to manipulate the machine context
    // of the interrupted program.

    // 1. As a third argument, the handler gets a ucontext_t
    //    structure, which contains the machine-independent and a
    //    machine-dependent part of the process state. In our case, we
    //    want to read AND manipulate the machine-dependent program
    //    counter.
    ucontext_t *ctx = (ucontext_t*)context;

    // 2. Extract the program counter from the uc_mcontext (machine-dependent part).
    uintptr_t pc = ctx->uc_mcontext.gregs[REG_RIP];
    syscall_write("sa_sigill: REG_RIP = main + 0x", pc - (uintptr_t)&main, 16);

    // 3. Manipulate the stored machine state by jumping 4 bytes
    //    forward. This all happens in the hope that the faulting
    //    address is 4 bytes long. Of course, for the variable-length
    //    AMD64 architecture, this is not true in general, but only for
    //    our use case.
    //
    //    If you want to have a proper illegal-instruction jumper, you
    //    would require a full x86 disassembler that tells you how
    //    long the invalid instruction actually is. Which is
    //    troublesome in itself. But hey, this is only for
    //    demonstration purposes.
    ctx->uc_mcontext.gregs[REG_RIP]  = pc + 4;
}


int main(void) {
    // We get the actual page-size for this system. On x86, this
    // always return 4096, as this is the size of regular pages on
    // this architecture. We need this in the SIGSEGV handler.
    PAGE_SIZE = sysconf(_SC_PAGESIZE);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags     = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);

    // Control-C sends a SIGINT
    sa.sa_sigaction = &sa_sigint;
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        perror("sigaction");
        return -1;
    }

    sa.sa_sigaction = &sa_sigsegv;
    if (sigaction(SIGSEGV, &sa, NULL) != 0) {
        perror("sigaction");
        return -1;
    }

    sa.sa_sigaction = &sa_sigill;
    if (sigaction(SIGILL, &sa, NULL) != 0) {
        perror("sigaction");
        return -1;
    }


    // We generate an invalid pointer that points _somewhere_! This is
    // undefined behavior, and we only hope for the best here. Perhaps
    // we should install a signal handler for SIGSEGV beforehand....
    uint32_t * addr = (uint32_t*)0xdeadbeef;

    // This will provoke a SIGSEGV
    *addr = 23;

    // Two ud2 instructions are exactly 4 bytes long
#define INVALID_OPCODE_32_BIT() asm("ud2; ud2;")

    // This will provoke a SIGILL
    INVALID_OPCODE_32_BIT();

    // Happy faulting, until someone sets the do_exit variable.
    // Perhaps the SIGINT handler?
    while(!do_exit) {
        sleep(1);
        addr += 22559;
        *addr = 42;
        INVALID_OPCODE_32_BIT();
    }

    { // Like in the mmap exercise, we use pmap to show our own memory
      // map, before exiting.
        char cmd[256];
        snprintf(cmd, 256, "pmap %d", getpid());
        printf("---- system(\"%s\"):\n", cmd);
        system(cmd);
    }

    return 0;
}
