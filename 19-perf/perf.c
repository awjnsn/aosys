#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <asm/unistd.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>
#include <emmintrin.h>
#include <limits.h>


#define ARRAY_SIZE(arr) (sizeof(arr)/sizeof(*(arr)))
#define die(msg) do { perror(msg); exit(EXIT_FAILURE); } while(0)

// Matrix Multiplication Functions
#include "matrix.c"

// When reading from the perf descriptor, the kernel returns an
// event record in the following format (if PERF_FORMAT_GROUP |
// PERF_FORMAT_ID are enabled).
// Example (with id0=100, id1=200): {.nr = 2, .values = {{41433, 200}, {42342314, 100}}}
typedef uint64_t perf_event_id; // For readability only
struct read_format {
    uint64_t nr;
    struct {
        uint64_t value;
        perf_event_id id; // PERF_FORMAT_ID
    } values[];
};

// Structure to hold a perf group
struct perf_handle {
    int group_fd;   // First perf_event fd that we create
    int nevents;    // Number of registered events
    size_t rf_size; // How large is the read_format buffer (derived from nevents)
    struct read_format *rf; // heap-allocated buffer for the read event
};

// Syscall wrapper for perf_event_open(2), as glibc does not have one
int sys_perf_event_open(struct perf_event_attr *attr,
                    pid_t pid, int cpu, int group_fd,
                    unsigned long flags) {
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

// Add a perf event of given (type, config) with default config. If p
// is not yet initialized (p->group_fd <=0), the perf_event becomes
// the group leader. The function returns an id that can be used in
// combination with perf_event_get.
perf_event_id perf_event_add(struct perf_handle *p, int type, int config) {
    struct perf_event_attr attr;

    memset(&attr, 0, sizeof(struct perf_event_attr));
    attr.type = type;
    attr.size = sizeof(struct perf_event_attr);
    attr.config = config;
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID;
    int fd = sys_perf_event_open(&attr, 0, -1,
                             p->group_fd > 0 ? p->group_fd : -1,
                             0);
    if (fd < 0) die("perf_event_open");
    if (p->group_fd <= 0)
        p->group_fd = fd;

    p->nevents ++;

    perf_event_id id;
    if (ioctl(fd, PERF_EVENT_IOC_ID, &id) < 0)
        die("perf/IOC_ID");
    return id;
}

// Resets and starts the perf measurement
void perf_event_start(struct perf_handle *p) {
    // Reset and enable the event group
    ioctl(p->group_fd, PERF_EVENT_IOC_RESET,  PERF_IOC_FLAG_GROUP);
    ioctl(p->group_fd, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
}

// Stops the perf measurement and reads out the event
void perf_event_stop(struct perf_handle *p) {
    // Stop the tracing for the whole event group
    ioctl(p->group_fd, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);

    // Allocate a read_format buffer if not done yet.
    if (p->rf == NULL) {
        p->rf_size = sizeof(uint64_t) + 2 * p->nevents * sizeof(uint64_t);
        p->rf = malloc(p->rf_size);
    }

    // get the event from the kernel. Our buffer should be sized exactly righ
    if (read(p->group_fd, p->rf, p->rf_size) < 0)
        die("read");
}


// After the measurement, this helper extracts the event counter for
// the given perf_event_id (which was returned by perf_event_add)
uint64_t perf_event_get(struct perf_handle *p, perf_event_id id) {
    for (unsigned i = 0; i < p->rf->nr; i++) {
        if (p->rf->values[i].id == id) {
            return p->rf->values[i].value;
        }
    }
    return -1;
}

int main(int argc, char* argv[]) {
    unsigned dim = 32;
    if (argc > 1) {
        dim = atoi(argv[1]);
    }
    if ((dim & (dim - 1)) != 0) {
        fprintf(stderr, "Given dimension must be a power of two\n");
        exit(EXIT_FAILURE);
    }

    // Create some matrices
    double *A  = create_random_matrix(dim);
    double *B  = create_random_matrix(dim);
    double *C0 = create_matrix(dim);
    double *C1 = create_matrix(dim);

    size_t msize = sizeof(double) * dim * dim;
    printf("matrix_size: %.2f MiB\n", msize / (1024.0 * 1024.0));

    // Create and initialize a new perf handle
    struct perf_handle p;
    memset(&p, 0, sizeof(p));

    // Create three new perf events that we want to monitor for our
    // matrix multiplication algorithms
    perf_event_id id_instrs =
        perf_event_add(&p, PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS);
    perf_event_id id_cycles =
        perf_event_add(&p, PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES);
    perf_event_id id_cache_miss =
        perf_event_add(&p, PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES);

    // Define an anonymous struct type to make our measurement code easier to read
    struct {
        char *name;
        void (*func)(unsigned, double *, double *, double*);
        double *result;
    } algorithms[] = {
        {"drepper", &matrixmul_drepper, C0},
        {"naive",   &matrixmul_naive,   C1},
    };

    for (unsigned i = 0; i < ARRAY_SIZE(algorithms); i++) {
        // Execute the matrix multiplication under perf tracing
        perf_event_start(&p);
        algorithms[i].func(dim, A, B, algorithms[i].result);
        perf_event_stop(&p);

        // Print out the results as a single line
        double instrs = perf_event_get(&p, id_instrs) / 1e6;
        double cycles = perf_event_get(&p, id_cycles) / 1e6;
        double misses = perf_event_get(&p, id_cache_miss) / 1e6;
        printf("%-10s %8.2fM instr, %8.2f instr-per-cycle, %8.2f miss-per-instr\n",
               algorithms[i].name, instrs, instrs/cycles, misses / instrs);
    }

    // Sanity Checking: are both result matrices equal (with a margin of 0.1%) ?
    for (unsigned i = 0; i < (dim*dim); i++) {
        double delta = 1.0 - C1[i]/C0[i];
        if (delta > 0.001 || delta < -0.001) {
            fprintf(stderr, "mismatch at %d: %f%%\n", i, delta * 100);
            return -1;
        }
    }
    return 0;
}
