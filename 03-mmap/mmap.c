#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdlib.h>

// We define a CPP macro to hide the ugliness of compiler attributes.
//
// - aligned: Normally, variables can be densely packed in the
//            data/BSS section. However, as we want to replace parts
//            of our data section with a file mapping, we have to obey
//            the MMU's granularity (4096 bytes).
#define PAGE_SIZE 4096
#define page_aligned __attribute__((aligned(PAGE_SIZE)))

// This is anonymous struct describes our persistent data and a
// variable with that type, named psec, is defined.
struct /* anonymous */ {
    // We mark the first variable in our struct as being page aligned.
    // This is equivalent to a start address an address ending in 3
    // hexadecimal zeros.
    //
    // By aligning the first variable in the struct, two things happen:
    //
    // 1. Global variables of struct persistent_section are placed at
    //    the beginning of a page.
    // 2. The size of the section is padded to the next multiple of
    //    PAGE_SIZE, such that array-address calculations and pointer
    //    arithmetik work correctly.
    page_aligned int persistent_start;

   // This is our persistent variable. We will use it as a counter,
   // how often the program was executed.
    int foobar;
} psec;

// For comparision, we also create another global variable, that is
// initialized from the program ELF on every program start.
int barfoo = 42;

int setup_persistent(char *fn) {
    // We first stat the file, before we open it with the O_CREAT
    // flag, whereby we can determine lateron, if the file has
    // existsted beforehand. 
    struct stat s;
    int rc = stat(fn, &s);
    int fd = open(fn, O_RDWR|O_CREAT, 0600);

    // We could not open/create the file. This is not good and we
    // cannot and do not want to recover from it.
    if (fd < 0)
        return -1;

    // If stat failed, the file did not exist beforehand, and we want
    // to initialize our backing storage. We also initialize the file,
    // if its size is different from out persistent size.
    if (rc == -1 || s.st_size != sizeof(psec)) {
        // First, we shrink/expand the file to the correct size. We
        // use ftruncate(2) for this
        if (ftruncate(fd, sizeof(psec)) != 0)
            return -1;

        // We copy the contents of the initialized persistent section
        // from the DRAM to the file, by writing it into the file descriptor
        if (write(fd, &psec, sizeof(psec)) != sizeof(psec))
            return -1;

        // We have initialized the file.
    }

    // We replace the memory at &psec with the contents of our file.
    // But unlike read(), resulting mapping is synchronized with the
    // file contents.
    //
    // addr: Where do we want our mapping to start. Usually this is
    //       only a hint.
    // length: We want to map that many bytes starting at addr
    // prot: Yes, we want that memory to be read and writable
    // flags/MAP_SHARED: The mapping should not be a private copy, but
    //       our modifications should be shared with everybody else.
    //       (this includes persistence)
    // flags/MAP_FIXED: The addr is not only a hint, but mmap should fail
    //       in case the OS cannot install a mapping there.
    // file: Where should the content for the mapping come from?
    // offset: offset within the file descriptor
    int *map = mmap(/* addr */  &psec,
                    /* length*/ sizeof(psec),
                    /* prot */  PROT_READ|PROT_WRITE,
                    /* flags */ MAP_SHARED|MAP_FIXED,
                    /* file */  fd,
                    /* offset */ 0);
    // mmap returns a special pointer (MAP_FAILED) to inidicate that
    // the operation did not succeed.
    if (map == MAP_FAILED)
        return -1;

    // This is interesting: We can close the file descriptor and our
    // mapping remains in place. Internally, the struct file is not
    // yet destroyed as the mapping keeps a reference to it.
    close(fd);

    return 0;
}

int main(int argc, char *argv[]) {
    printf("psec: %p--%p\n", &psec, &psec + 1);
    // Install the persistent mapping
    if (setup_persistent("mmap.persistent") == -1) {
        perror("setup_persistent");
        return -1;
    }

    // For psec.foobar, we see that each invokation of the programm will
    // yield an incremented result.
    // For barfoo, which is *NOT* in the persistent section, we will
    // always get the same result.
    printf("foobar(%p) = %d\n", &psec.foobar, psec.foobar++);
    printf("barfoo(%p) = %d\n", &barfoo, barfoo++);

    {// This is ugly and you should not do this in production code.

        // In order to see the memory mappings of the currently
        // running process, we use the pmap (for process-map) tool to
        // query the kernel (/proc/self/maps)
        char cmd[256];
        snprintf(cmd, 256, "pmap %d", getpid());
        printf("---- system(\"%s\"):\n", cmd);
        system(cmd);
    }

    return 0;
}
