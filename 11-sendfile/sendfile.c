#define _GNU_SOURCE
#include <stdio.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/types.h>
#include <assert.h>
#include <limits.h>
#include <sys/sendfile.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>


#define die(msg) do { perror(msg); exit(EXIT_FAILURE); } while(0)

ssize_t copy_write(int fd_in, int fd_out, int *syscalls) {
    ssize_t ret = 0;
    *syscalls = 0;
    // For the read/write copy method, we require a buffer. To not
    // hinder performance, we use a static function variable to
    // allocate the buffer once (it is never freed.
    static size_t bufsize = 128 * 1024; // GNU cp uses 128k Buffers
    static char  *buffer = NULL;
    if (!buffer) {
        buffer = malloc(bufsize);
        if(!buffer) die("malloc");
    }

    // The actual copy loop
    do {
        // Read up to bufsize bytes.
        int len = read(fd_in, buffer, bufsize);
        if (len < 0) die("read");
        (*syscalls) ++;      // read is a system call
        if (len == 0) break; // end-of file is reached

        // As write could be short, we loop until all bytes are
        // written to the output file descriptor.
        int written = 0;
        while (written < len) {
            int wlen = write(fd_out, buffer+written, len-written);
            if (wlen < 0) die("write");
            (*syscalls) ++;
            written += wlen;
        }
        // Account for the copied bytes.
        ret += len;
    } while(1);

    return ret;
}

ssize_t copy_sendfile(int fd_in, int fd_out, int *syscalls) {
    ssize_t ret = 0;
    *syscalls = 0;
    
    // For sendfile, we do not require a buffer, but we can directly
    // call sendfile on both descriptors. Please note how insane the
    // order of the arguments (dest, source) is!
    do {
        int len = sendfile(fd_out, fd_in, NULL, INT_MAX);
        if (len < 0) die("sendfile");
        (*syscalls)++;
        if (len == 0) break; // end of file

        // Account for the copied bytes
        ret += len;
    } while(1);
    return ret;
}

// This function measures the given copy implementation.
// fd_in:  file descriptor to copy from
// fd_out: file descriptor to copy to
// banner: Just a nice string to print the output
// copy:   The copy implementation
// returns the number of bytes per second
double measure(int fd_in, int fd_out, char *banner, ssize_t (*copy)(int, int, int*)) {
    // First, we reset our file descriptors.
    // fd_in:  seek to position zero
    // fd_out: truncate the file to zero bytes.
    if (lseek(fd_in, 0, SEEK_SET) < 0) die("lseek");
    if (ftruncate(fd_out, 0) < 0)      die("ftruncate");

    // Measure the start time.
    struct timespec start, end;
    if (clock_gettime(CLOCK_REALTIME, &start) < 0)
        die("clock_gettime");

    // Perform the actual copy. We give the copy function also a
    // pointer to syscalls, where the implementation should count the
    // number of issued system calls.
    int syscalls;
    ssize_t bytes = copy(fd_in, fd_out, &syscalls);


    // Measure the end time
    if (clock_gettime(CLOCK_REALTIME, &end) < 0)
        die("clock_gettime");

    // Calculate the time delta between both points in time.
    double delta = end.tv_sec - start.tv_sec;
    delta += (end.tv_nsec - start.tv_nsec) / 1e9;

    // Print out some nicely formatted message
    printf("[%10s] copied with %.2f MiB/s (in %.2f s, %d syscalls)\n",
           banner, (bytes /delta) / 1024.0 / 1024.0, delta, syscalls);

    return bytes / delta;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s FILE\n", argv[0]);
        return -1;
    }
    // As input file, we open the user-specified file from the command
    // line as a read only file.
    int fd_in = open(argv[1], O_RDONLY);
    if (fd_in < 0) die("open");

    // As an output, we create an anonymous in-memory file. By using
    // such an in-memory file, do not measure the influence of on-disk
    // file systems.
    int fd_out = memfd_create("target", 0);

    // We will run for ten rounds, unless the user specified something
    // else in the ROUNDS environment variable.
    char *ROUNDS = getenv("ROUNDS");
    int rounds = atoi(ROUNDS ? ROUNDS : "10");

    // We run the copy_write algorithm once to warm up the buffer
    // cache. With this, the input file should now, if small enough,
    // reside in in the buffer cache.
    int dummy;
    copy_write(fd_in, fd_out, &dummy);

    // The actual measurement
    double sendfile = 0, write = 0;
    for (int i = 0; i < rounds; i++) {
        sendfile += measure(fd_in, fd_out, "sendfile", copy_sendfile);
        write    += measure(fd_in, fd_out, "read/write", copy_write);
    }

    // Print the average MiB/s for both copy algorithms
    printf("sendfile: %.2f MiB/s, read/write: %.2f MiB/s\n",
           sendfile/rounds/(1024*1024),
           write/rounds/(1024*1024));
}
