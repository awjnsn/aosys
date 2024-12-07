#define _GNU_SOURCE
#include <errno.h>
#include <memory.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#define die(msg)                                                               \
    do                                                                         \
    {                                                                          \
        perror(msg);                                                           \
        exit(EXIT_FAILURE);                                                    \
    } while (0)

int main()
{
    // FIXME: Read in lines (HINT: getline(3))
    // FIXME: Sort lines (HINT: qsort(3))
    // FIXME: Dump lines with writev(2)
}
