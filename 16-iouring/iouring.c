#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <string.h>
#include <linux/io_uring.h>
#include <errno.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <stdbool.h>
#include <assert.h>


#define die(msg) do { perror(msg); exit(EXIT_FAILURE); } while(0)

#define CANARY 0xdeadbeef

////////////////////////////////////////////////////////////////
// Buffer Allocator

struct buffer {
    union {
        struct {
            // If a buffer is in our free list, we use the buffer
            // memory (through the union) to have a single-linked list
            // of available buffers
            struct buffer * next;
            // In order to have a little safety guard, we place a
            // canary value here
            unsigned canary;
        };
        // The data for the buffer
        char data[4096];
    };
};

static_assert(sizeof(struct buffer) == 4096);

// We have a stack of empty buffers. At process start, the
// free-buffer stack is empty
static struct buffer *free_buffers = NULL;

// Allocate a buffer
struct buffer* alloc_buffer() {
    struct buffer *ret;
    // If the stack is empty, we allocate a new buffer and give it to
    // the caller. We use posix_memalign(3) for this, as the
    // destination buffer for read operations on an O_DIRECT file
    // descriptor have to be aligned.
    if (free_buffers == NULL) {
        if (posix_memalign((void**)&ret, 512, sizeof(struct buffer)) < 0)
            die("posix_memalign");
        return ret;
    }

    // Our free stack contains a buffer. Pop it; check the canary; and
    // return it to the user.
    ret = free_buffers;
    free_buffers = free_buffers->next; // Pop
    assert(ret->canary == CANARY); // Test the canary
    ret->canary = 0;
    return ret;
}

// Free a buffer to the free-buffer stack and set the canary value,
// which we use to detect memory corruptions. For example, if the
// kernel overwrites a buffer, while it is in the free stack (both
// free and allocated), the canary will detect that.
void free_buffer(struct buffer *b) {
    b->canary = CANARY;
    b->next   = free_buffers;
    free_buffers = b; // Push
}

////////////////////////////////////////////////////////////////
// Helpers for using I/O urings

// We have our own system call wrappers for io_uring_setup(2) and
// io_uring_enter(2) as those functions are also provided by the
// liburing. To minimize confusion, we prefixed the helpers with `sys_'
int sys_io_uring_setup(unsigned entries, struct io_uring_params *p) {
    return (int) syscall(__NR_io_uring_setup, entries, p);
}

int sys_io_uring_enter(int ring_fd, unsigned int to_submit,
                   unsigned int min_complete, unsigned int flags) {
    return (int) syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete,
                         flags, NULL, 0);
}

// Loads and stores of the head and tail pointers of an io_uring require
// memory barriers to be fully synchronized with the kernel.
//
// For a very good and detailed discussion of load-acquire and
// store-release, we strongly recommend the LWN article series on
// lockless patterns: https://lwn.net/Articles/844224/
//
// If you have not yet understood that memory is a distributed system
// that has a happens-before relation, than read(!) that series.
#define store_release(p, v)                                    \
    atomic_store_explicit((_Atomic typeof(*(p)) *)(p), (v), \
                          memory_order_release)
#define load_aquire(p)                    \
    atomic_load_explicit((_Atomic typeof(*(p)) *)(p),   \
                         memory_order_acquire)


struct ring {
    // The file descriptor to the created uring.
    int ring_fd;

    // The params that we have received from the kernel
    struct io_uring_params params;

    // We perform our own book keeping how many requests are currently
    // in flight.
    unsigned in_flight;

    // The Submission Ring (mapping 1)
    unsigned *sring;       // An array of length params.sq_entries
    unsigned *sring_tail;  // Pointer to the tail index
    unsigned  sring_mask;  // Apply this mask to (*sring_tail) to get the next free sring entry

    // The SQE array (mapping 2)
    //
    // An SQE is like a system call that you prepare in this array and
    // then push its index into the submission ring (please search for
    // the term "indirection array" in io_uring(7)
    struct io_uring_sqe *sqes;

    // The Completion Queue (mapping 3)
    unsigned *cring_head; // Pointer to the head index (written by us, read by the kernel)
    unsigned *cring_tail; // Pointer to the tail index (written by the kernel, read by us)
    unsigned  cring_mask; // Apply this mask the head index to get the next available CQE
    struct io_uring_cqe *cqes; // Array of CQEs
};

// Map io_uring into the user space. This includes:
// - Create all three mappings (submission ring, SQE array, and completion ring)
// - Derive all pointers in struct ring
struct ring ring_map(int ring_fd, struct io_uring_params p) {
    struct ring ring = {
        .ring_fd = ring_fd,
        .params = p,
        .in_flight = 0
    };

    // The Submission Ring (mapping 1)
    int sring_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    void *sq_ptr = mmap(0, sring_sz, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE,
                        ring_fd, IORING_OFF_SQ_RING);
    if (sq_ptr == MAP_FAILED) die("mmap");

    ring.sring_tail = sq_ptr + p.sq_off.tail;
    ring.sring_mask = *(unsigned *)(sq_ptr + p.sq_off.ring_mask);
    ring.sring      = sq_ptr + p.sq_off.array;

    // The SQE array (mapping 2)
    int sqe_sz = p.sq_entries * sizeof(struct io_uring_sqe);
    void *sqes = mmap(0, sqe_sz, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_POPULATE,
                      ring_fd, IORING_OFF_SQES);
    if (sqes == MAP_FAILED) die("mmap");
    ring.sqes = sqes;

    // The Completion queue (mapping 3)
    int cring_sz = p.cq_off.cqes  + p.cq_entries * sizeof(struct io_uring_cqe);
    void *cq_ptr = mmap(0, cring_sz, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE,
                        ring_fd, IORING_OFF_CQ_RING);
    if (cq_ptr == MAP_FAILED) die("mmap");
    ring.cring_head = cq_ptr + p.cq_off.head;
    ring.cring_tail = cq_ptr + p.cq_off.tail;
    ring.cring_mask = *(unsigned *)(cq_ptr + p.cq_off.ring_mask);
    ring.cqes       = cq_ptr + p.cq_off.cqes;
    return ring;
}

// Submit up to count random-read SQEs into the given file with a
// _single_ system call. The function returns the number of actually
// submitted random reads.
unsigned submit_random_read(struct ring *R, int fd, ssize_t fsize, unsigned count) {
    // With a single io_uring_enter(2), we can submit only as many
    // SQEs as there are sq_entries in the mmap'ed ring.
    if (count > R->params.sq_entries)
        count = R->params.sq_entries;

    // Read the submission tail pointer with a load acquire. Since this
    // pointer is only written by user space an from our thread, this
    // load_acquire is *OPTIONAL*.
    unsigned tail = load_aquire(R->sring_tail);

    // Submit count SQEs. For each SQE, we push the tail index by one
    // element
    for (unsigned i = 0; i < count; i++) {
        // As we only increment the tail pointer (and never wrap it
        // around), we use the kernel provided mask to do the 'modulo'
        // operation. For an 32-entry sring, the sring_mask is 31.
        unsigned index = tail & R->sring_mask;
        tail ++;

        // We use an 1:1 mapping between sring entries and SQE
        // entries. However, this is not mandatory and the user can
        // implement its own SQE management between SQE preparation
        // and submission. Please note, that the kernel will copy the
        // SQE into userspace on submit, whereby the SQE is
        // immediately reusable.
        struct io_uring_sqe *sqe = &R->sqes[index];

        // Reinitialize all SQEs with zeros
        memset(sqe, 0, sizeof(struct io_uring_sqe));

        // Allocate a destination buffer and roll the dice which
        // block from the file should be read from disk.
        struct buffer *buffer = alloc_buffer();
        unsigned block_size   = sizeof(buffer->data);
        unsigned block        = random() % (fsize / block_size);

        // Prepare an SQE
        sqe->opcode = IORING_OP_READ;
        // The source block (fd, offset
        sqe->fd     = fd;
        sqe->off    = block * block_size;
        // The destination buffer
        sqe->addr   = (unsigned long) buffer;
        sqe->len    = block_size;
        // We set the pointer to our destination buffer as the
        // user_data field, which will be returned in the CQE. With
        // that information, we can free that buffer again.
        sqe->user_data = (unsigned long) buffer;

        // printf("SQE[%d]: opcode=%d, fd=%d, addr=%p, len=%x off=%llx user_data=%p\n",
        //        index, sqe->opcode, sqe->fd, (void*)sqe->addr, sqe->len, sqe->off, (void*)sqe->user_data);

        // Insert the prepared SQE into the submission queue.
        R->sring[index] = index;
    }

    // With a single store_release, we forward the actual tail
    // pointer, whereby the prepared SQEs become visible to the
    // kernel. After this, the kernel could already process the SQEs,
    // but we still have to inform him about this update
    store_release(R->sring_tail, tail);

    // We use io_uring_enter (to_submit=count, min_completions=0)
    unsigned submitted = sys_io_uring_enter(R->ring_fd, count, 0, 0);
    if (submitted < 0)
        die("io_uring_enter");
    assert(submitted == count);

    // Record that we now have more requests in flight.
    R->in_flight += submitted;

    return submitted;
}

// Reap one CQE from the completion ring and copy the CQE to *cqe. If
// no CQEs are available (*cring_head == *cring_tail), this function
// returns 0.
int reap_cqe(struct ring *R, struct io_uring_cqe *cqe) {
    // Read the head index of the completion queue with an
    // load-acquire. Thereby, we also capture head updates that were
    // issued on other CPUs on weak memory
    unsigned head = load_aquire(R->cring_head);

    // If head==tail, the completion ring is empty
    if (head == *R->cring_tail)
        return 0;

    // Extract the CQE from the completion queue by copying
    *cqe = R->cqes[head & R->cring_mask];

    // Forward the head pointer with a store-release
    store_release(R->cring_head, head+1);

    // printf("CQE[%d]: res: %d user_data: %lld, flags=%d\n",
    //        index, cqe->res, cqe->user_data, cqe->flags);

    if (cqe->res < 0) {
        errno = -1 * cqe->res;
        die("CQE/res");
    }
    return 1;
}

// This function uses reap_cqe() to extract a filled buffer from the
// uring. If wait is true, we wait for an CQE with
// io_uring_enter(min_completions=1, IORING_ENTER_GETEVENTS) if
// necessary.
struct buffer * receive_random_read(struct ring *R, bool wait) {
    struct buffer *buf;
    struct io_uring_cqe cqe;
    // Optimistic reap without issuing a system call first.
    if (reap_cqe(R, &cqe) > 0) {
        goto extract_buffer;
    } else if (wait) {
        // If we are allowed to wait, we wait for an CQE
        sys_io_uring_enter(R->ring_fd, 0, 1, IORING_ENTER_GETEVENTS);

        // This reap should always succeed as we have waited
        if (reap_cqe(R, &cqe) > 0)
            goto extract_buffer;
    }
    return NULL;
 extract_buffer:
    // Derive a buffer pointer from the cqe._user data, perform some
    // safety checks and decrement the in_flight counter.
    buf = (struct buffer*) cqe.user_data;
    assert(cqe.res == sizeof(buf->data) && "Short Read");
    assert(buf != NULL && "Invalid User Data");
    R->in_flight--;
    return buf;
}

int main(int argc, char *argv[]) {
    // Argument parsing
    if (argc < 3) {
        fprintf(stderr, "usage: %s SQ_SIZE FILE\n", argv[0]);
        return -1;
    }

    int sq_size  = atoi(argv[1]);
    char *fn     = argv[2];

    // Initialize the random number generator with the current time
    // with gettimeofday(2). We use the nanoseconds within this second
    // to get some randomness as a seed for srand(3).
    struct timeval now;
    gettimeofday(&now, NULL);
    srand(now.tv_usec);

    // Open the source file and get its size
    int fd = open(fn, O_RDONLY | O_DIRECT);
    if (fd < 0) die("open");

    struct stat s;
    if (fstat(fd, &s) < 0) die("stat");
    ssize_t fsize = s.st_size;

    struct ring R = {0};
    // Create an io_uring with default parameters and of with sq_size entries
    struct io_uring_params params = { 0 };
    int ring_fd   = sys_io_uring_setup(sq_size, &params);
    // Map the ring data structures to the user space
    R = ring_map(ring_fd, params);

    // Some debug output
    printf("init_ring: sq_size=%d\n", sq_size);
    printf("SQ: %d entries (%p), ring: %p\n", R.params.sq_entries, R.sqes, R.sring);
    printf("CQ: %d entries ring: %p\n", R.params.cq_entries, R.cqes);
    
    // A per-second statistic about the performed I/O
    unsigned read_blocks = 0; // Number of read blocks
    ssize_t  read_bytes  = 0; // How many bytes where read
    while(1) {

        // For the first loop iteration, we wait for an CQE.
        bool wait = true;
        while(R.in_flight > 0) {
            struct buffer *buf = receive_random_read(&R, wait);
            assert(buf != NULL || !wait);
            if (!buf) break;

            // Update the statistic
            read_blocks += 1;
            read_bytes  += sizeof(buf->data);

            // Free the buffer again
            free_buffer(buf);

            wait = false;
        }

        unsigned to_submit = sq_size - R.in_flight;
        submit_random_read(&R, fd, fsize, to_submit);


        // Every second, we output a statistic ouptu
        struct timeval now2;
        gettimeofday(&now2, NULL);
        if (now.tv_sec < now2.tv_sec) {
            printf("in_flight: %d, read_blocks/s: %.2fK, read_bytes: %.2f MiB/s\n",
                   R.in_flight, read_blocks/1000.0, read_bytes / (1024.0 * 1024.0));
            read_blocks = 0;
            read_bytes = 0;
        }
        now = now2;
    }
}
