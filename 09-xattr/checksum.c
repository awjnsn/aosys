#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/xattr.h>
#include <stdint.h>

#define die(msg) do { perror(msg); exit(EXIT_FAILURE); } while(0)

// An helper function that maps a file to memory and returns its
// length in *len and an open file descriptor in *fd.
// On error, the function should return the null pointer;
char * map_file(char *fn, ssize_t *len, int *fd) {
    // We use stat to check if the file exists and to determine which
    struct stat s;
    int rc = stat(fn, &s);
    if (rc < 0) return NULL;

    // Open the file read only.
    int _fd = open(fn, O_RDONLY);
    if (! _fd) return NULL;

    // Map the file as shared and read only somewhere to memory
    char *map = mmap(NULL, s.st_size, PROT_READ, MAP_SHARED, _fd, 0);
    // mmap returns a special pointer (MAP_FAILED) to indicate that
    // the operation did not succeed.
    if (map == MAP_FAILED)
        return NULL;

    // We return three value to our caller
    *len = s.st_size;
    *fd  = _fd;
    return map;
}

// A (very) simple checksum function that calculates and additive checksum over a memory range.
uint64_t calc_checksum(void *data, size_t len) {
    uint64_t checksum = 0;

    // First sum as many bytes as uint64_t as possible
    uint64_t *ptr = (uint64_t*)data;
    while ((void *)ptr < (data + len)) {
        checksum += *ptr++;
    }

    // The rest (0-7 bytes) are added byte wise.
    char *cptr = (char*)ptr;
    while ((void*)cptr < (data+len)) {
        checksum += *cptr;
    }

    return checksum;
}

int main(int argc, char *argv[]) {
    // The name of the extended attribute where we store our checksum
    const char *xattr = "user.checksum";

    // Argument filename
    char *fn;

    // Should we reset the checksum?
    bool reset_checksum;

    // Argument parsing
    if (argc == 3 && strcmp(argv[1], "-r") == 0) {
        reset_checksum = true;
        fn = argv[2];
    } else if (argc == 2) {
        reset_checksum = true;
        fn = argv[1];
    } else {
        fprintf(stderr, "usage: %s [-r] <FILE>\n", argv[0]);
    }
    // We map the file to memory, get the length and an open file
    // descriptor. Later on we will only use the file-descriptor based
    // variants of {set,remove,get}xattr as that avoids the TOCTOU
    // (time-of-check, time-of-update) problem.
    ssize_t len;
    int fd;
    char *data = map_file(fn, &len, &fd);
    if (!data) die("map_file");

    // If we are instructed to remove the checksum, we use
    // fremovexattr to delete it. For the error checking, we allow it
    // that the checksum was not set beforehand.
    if (reset_checksum) {
        int rc = fremovexattr(fd, xattr);
        if (rc < 0 && errno != ENODATA) {
            die("fremovexattr");
        }
        return 0;
    }

    // We are sure that the file is mapped, so we can calcucate the
    // checksum over the memory mapped file.
    uint64_t checksum = calc_checksum(data, len);
    printf("current_checksum: %lx\n", checksum);

    // The final return code
    int ret  = 0;

    // We get the old checksum from the file. Please note that we
    // store the checksum as the raw 8 bytes of the uint64_t
    uint64_t old_checksum;
    if (fgetxattr(fd, xattr, &old_checksum, sizeof(old_checksum)) != sizeof(old_checksum)) {
        printf("old_checksum: NULL\n");
    } else {
        printf("previous_checksum: %lx\n", old_checksum);

        // On checksum mismatch, we will return an error.
        if (checksum != old_checksum) {
            fprintf(stderr, "checksum mismatch!");
            ret = -1;
        }
    }

    // In the end, we override the current checksum.
    if (fsetxattr(fd, xattr, &checksum, sizeof(checksum), 0) != 0) {
        die("fsetxattr");
    }
    return ret;
}
