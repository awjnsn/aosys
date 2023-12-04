#include <sys/types.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <linux/futex.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <stdbool.h>

////////////////////////////////////////////////////////////////
// Layer 0: Futex System Call Helpers
////////////////////////////////////////////////////////////////

/* We provide you with a few wrapper function to invoke the described
   futex system call as it is not directly exposed by the GNU C
   library.

   We use the atomic_int type here as it is 32-bit wide on all
   platforms of interest.

*/
int futex(atomic_int *addr, int op, uint32_t val,
          struct timespec *ts, uint32_t *uaddr2, uint32_t val3) {
    return syscall(SYS_futex, addr, op, val, ts, uaddr2, val3);
}

int futex_wake(atomic_int *addr, int nr) {
    return futex(addr, FUTEX_WAKE, nr, NULL, NULL, 0);
}

int futex_wait(atomic_int *addr, int val) {
    return futex(addr, FUTEX_WAIT, val, NULL, NULL, 0);
}

////////////////////////////////////////////////////////////////
// Layer 1: Semaphore Abstraction
////////////////////////////////////////////////////////////////

/* Initialize the semaphore. This boils down to setting the referenced
   32-bit word with the given initval */
void sem_init(atomic_int *sem, unsigned initval) {
    atomic_init(sem, initval);
}

/* The semaphores decrement operation tries to decrement the given
   semaphore. If the semaphore counter is larger than zero, we just
   decrement it. If it is already zero, we sleep until the value
   becomes larger than zero and try decrementing it again. */
void sem_down(atomic_int *sem) {
    
    // We have to use a loop here, as there can be a race between old
    // and new decrementors after another thread wakes up the old
    // decrementor.
    while (true) {
        // We retrieve the current value and try to decrement it if it
        // is larger than zero.
        int value = atomic_load(sem);
        if (value > 0) {
            // We use compare and exchange here to attempt the
            // decrement here. We hope that sem is still value. In
            // this case the compare and exchange updates the
            // semaphore to (value - 1).
            if (atomic_compare_exchange_strong(sem, &value, value - 1) == true)
                break; // Compare-and-Exchange succeeded!
        } else {
            // We wait, if the value is still zero!
            futex_wait(sem, 0);
        }
    }
}

/* The semaphore increment operation increments the counter and wakes
   up one waiting thread, if there is the possibility of waiting
   threads. */
void sem_up(atomic_int *sem) {
    // Get the old value and increment the semaphore unconditionally.
    int prev = atomic_fetch_add(sem, 1);

    // Only if the old value was zero there is the possibility of
    // waiting threads. Only in this case, we invoke the (expensive)
    // system-call.
    if (prev == 0)
        futex_wake(sem, 1);
}

////////////////////////////////////////////////////////////////
// Layer 2: Semaphore-Synchronized Bounded Buffer
////////////////////////////////////////////////////////////////

// Calculate the number of elements in a statically allocated array.
#define ARRAY_SIZE(arr) (sizeof(arr)/sizeof(*(arr)))

struct bounded_buffer {
    // We use two semaphores to count the number of empty slots and
    // the number of valid elements in our bounded buffer. Initialize
    // these values accordingly.
    atomic_int slots;
    atomic_int elements;

    // We use another semaphore as a binary semaphore to synchronize
    // the access to the data and meta-data of our bounded buffer
    atomic_int lock;
    unsigned int read_idx;  // Next slot to read
    unsigned int write_idx; // Next slot to write

    // We have place for three pointers in our bounded buffer.
    void * data[3];
};

void bb_init(struct bounded_buffer *bb) {

    // Binary Semaphore (used as Mutex): We initialize it with 1 as
    // the mutex is currently free. If someone wants to enter the
    // critical section, the semaphore is decremented.
    sem_init(&bb->lock, 1);

    // Counting semaphore: We use these semaphores to count the number
    // of empty slots (initally 3) and the number of valid elements
    // (initially 0) in our bounded buffer.
    sem_init(&bb->slots,    ARRAY_SIZE(bb->data));
    sem_init(&bb->elements, 0);

    // We start reading and writing at data[0]
    bb->read_idx = 0;
    bb->write_idx = 0;
}

void * bb_get(struct bounded_buffer *bb) {
    void *ret = NULL;

    // Before, we get an element, we have to ensure that there is at
    // least one element in the bounded buffer. For this, we decrement
    // the elements semaphroe
    sem_down(&bb->elements);

    // We retrieve the element from the data field using the read_idx.
    // As these indices are shared between threads and processes, we
    // synchronize the access to it with our binary semaphore.
    
    { // Critical Section (protected by bb->lock)
        sem_down(&bb->lock);

        ret = bb->data[bb->read_idx];
        bb->read_idx = (bb->read_idx + 1) % ARRAY_SIZE(bb->data);

        sem_up(&bb->lock);
    }

    // After we have removed the element, more slots are empty.
    // Therefore, we increment that semaphore, which potentially wakes
    // up threads that wait in `bb_put()`.
    sem_up(&bb->slots);

    return ret;
}

void bb_put(struct bounded_buffer *bb, void *data) {

    // bb_put performs almost the same operation as bb_put, with a few
    // differences:
    // - We first decrement the slots semaphore to allocate an empty slot
    // - We use the write_idx to determine the written slot
    // - We increment the number of elements afterwards.
    sem_down(&bb->slots);

    { // Critical Section
        sem_down(&bb->lock);

        bb->data[bb->write_idx] = data;
        bb->write_idx = (bb->write_idx + 1) % ARRAY_SIZE(bb->data);

        sem_up(&bb->lock);
    }

    sem_up(&bb->elements);

}


int main() {
    // First, we use mmap to establish a piece of memory that is
    // shared between the parent and the child process. The mapping is
    // 4096 bytes large a resides at the same address in the parent and the child process.
    char *shared_memory = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                               MAP_ANONYMOUS | MAP_SHARED, -1, 0);
    if (shared_memory == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    // We place a semaphore and a bounded buffer instance in our shared memory.
    atomic_int *semaphore     = (void *) &shared_memory[0];
    struct bounded_buffer *bb = (void *) &shared_memory[sizeof(atomic_int)];
    (void)bb;

    // We use this semaphore as a condition variable. The parent
    // process uses sem_down(), which will initially result in
    // sleeping, until the child has initialized the bounded buffer
    // and signals this by sem_up(semaphore).
    sem_init(semaphore, 0);

    pid_t child = fork();
    if (child == -1) {
        perror("fork");
        return -1;
    }

    if (child != 0) {
        ////////////////////////////////////////////////////////////////
        // Parent

        // Wait until the child has initialized the bounded buffer
        sem_down(semaphore);

        printf("Child has initialized the bounded buffer\n");

        char *element;
        do {
            element = bb_get(bb);
            printf("Parent: %p = '%s'\n", element, element);
        } while(element != NULL);
    } else {
        ////////////////////////////////////////////////////////////////
        // Child
        char *data[] = {
            "Hello", "World", "!", "How", "are", "you", "?"
        };
        (void)data;
        sleep(1);
        bb_init(bb);
        printf("Child: We initialized the bounded buffer\n");
        sem_up(semaphore);
        for (unsigned int i = 0; i < 9; i ++) {
            bb_put(bb, (void*) data[i % ARRAY_SIZE(data)]);

            if (i > 5) sleep(1);
        }
        bb_put(bb, NULL);
    }

    return 0;
}

