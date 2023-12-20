#define _GNU_SOURCE
#include <sys/uio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#include <Python.h>

#define die(msg) do { perror(msg); exit(EXIT_FAILURE); } while(0)

// Read memory from pid's memory, starting at ptr and len bytes.
// Returns an heap-allocated buffer
void *peek(pid_t pid, const void *ptr, size_t len) {
    struct iovec local[1];
    struct iovec remote[1];

    void    * buffer = malloc(len);
    if (!buffer) die("malloc");

    remote[0].iov_base = (void*)ptr;
    remote[0].iov_len = len;
    local[0].iov_base = buffer;
    local[0].iov_len = len;

    ssize_t nread = process_vm_readv(pid, local, 1, remote, 1, 0);
    if (nread != len)
        die("process_vm_readv");

    return buffer;
}

// Copy memory from this process to the remote process (pid).
// Remote Address: ptr
// Local  Address: buffer
// Length: len
void poke(pid_t pid, void *ptr, void *buffer, size_t len) {
    struct iovec local[1];
    struct iovec remote[1];

    remote[0].iov_base = ptr;
    remote[0].iov_len = len;
    local[0].iov_base = buffer;
    local[0].iov_len = len;

    ssize_t nread = process_vm_writev(pid, local, 1, remote, 1, 0);
    if (nread != len)
        die("process_vm_readv");
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s PID ADDR\n", argv[0]);
        return -1;
    }
    pid_t pid = atoi(argv[1]); (void)pid;
    void *ptr;
    int n = sscanf(argv[2], "0x%p", &ptr);
    if (n != 1) die("sscanf");

    // We get the object, which contains a pointer to its type object
    PyObject *obj      = peek(pid, ptr, sizeof(PyObject));
    // Get the type object
    PyTypeObject *type = peek(pid, Py_TYPE(obj), sizeof(PyTypeObject));
    // Get (parts of) the type name
    char *typename     = peek(pid, type->tp_name, 16);
    typename[15] = 0; // Make sure we have an zero terminated string

    // Some debug output
    printf("  PyObject @ %p: refcount=%ld, type=%s\n", ptr, Py_REFCNT(obj), typename);

    if (strcmp(typename, "float") == 0) {
        // If we have a float, we peek the object again with the correct size
        PyFloatObject *D = peek(pid, ptr, sizeof(PyFloatObject));

        // Print the floating point value
        printf("  PyFloatObject: ob_fval=%f\n", D->ob_fval);

        // Square the value
        D->ob_fval *= D->ob_fval;

        // Write the object back to python memory
        poke(pid, ptr, D, sizeof(PyFloatObject));
    }

    if (strcmp(typename, "int") == 0) {
        // Same procedure for PyLongObject. Noteworthy here is, that
        // Python supports arbitrary large integers. However, we only
        // manipulate the lower 16 bits here.
        PyLongObject *L = peek(pid, ptr, sizeof(PyLongObject));
        printf("  PyLongObject: ob_digit[0] = 0x%x\n", L->ob_digit[0]);
        L->ob_digit[0] &= ~0xffff;
        L->ob_digit[0] |= 0xabba;
        poke(pid, ptr, L, sizeof(PyLongObject));
    }
}
