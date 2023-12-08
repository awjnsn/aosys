#define _GNU_SOURCE
#include <string.h>
#include <sys/time.h>
#include <errno.h>
#include <stdio.h>
#include <sys/uio.h>
#include <stdio.h>
#include <memory.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>

#define die(msg) do { perror(msg); exit(EXIT_FAILURE); } while(0)


// We define ourselves a helper structure to have a growable iovec
// structure. If we would use C++, we could simply use std::vector.
struct dynamic_iovec {
    struct iovec *vec; // An heap allocated iovector
    int count;         // ... that currently contains count elements
    int capacity;      // ... that can hold up to capacity elements
};

// An initializer list to correctly initialize an dynamic_iovec instance
#define DYNAMIC_IOVEC_INIT() {.vec = NULL, .count=0, .capacity=0}

// Adds an element to our dynamic iovector. Grow it if there is not enough capacity.
void dynamic_iovec_add(struct dynamic_iovec *vec, void* data, ssize_t len) {
    // Resize our I/O vector if the capacity is not sufficiently
    // large. This test also works if capacity and count are zero.
    if ((vec->count + 1) > vec->capacity) {
        vec->capacity = 2 * (vec->count + 1);
        vec->vec = realloc(vec->vec, vec->capacity * sizeof(struct iovec));
        if (!vec) die("realloc");
    }

    // Add the element to the I/O vector
    vec->vec[vec->count].iov_base = data;
    vec->vec[vec->count].iov_len  = len;
    vec->count++;
}

// We cast the pointers from void to struct iovec
// and utilize strcmp(3) with their iov_bases for comparison
int compare_lines(const void *p1, const void *p2) {
    struct iovec *v1 = (struct iovec *)p1;
    struct iovec *v2 = (struct iovec *)p2;
    
    return strcmp((char *)v1->iov_base, (char *)v2->iov_base);
}

int main() {
    // We stack-allocate an dynamic_iovec to hold our lines.
    // (one line = tuple of char * and length).
    struct dynamic_iovec lines = DYNAMIC_IOVEC_INIT();
    
    // We use getline(3) to read lines from stdin. Please consult the
    // man page to understand the semantic of dummy!
    ssize_t nread;
    size_t dummy;
    char *line;
    while ((dummy = 0, nread = getline(&line, &dummy, stdin)) != -1) {
        // Add the line to the dynamic vector
        dynamic_iovec_add(&lines, line, nread);
    }

    // We sort the I/O vectors by using qsort(3) with compare_lines 
    // as the comparison function.
    qsort((void *)lines.vec, (size_t)lines.count, sizeof(struct iovec), compare_lines);

    // After sorting, we use a single system call to dump all lines
    // in one go.
    writev(STDOUT_FILENO, lines.vec, lines.count);

}
