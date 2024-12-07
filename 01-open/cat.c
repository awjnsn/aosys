#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    // For cat, we have to iterate over all command-line arguments of
    // our process, where argv[0] is our program binary itself ("./cat").
    for (int idx = 1; idx < argc; idx++)
    {
        printf("argv[%d] = %s\n", idx, argv[idx]);
    }

    return 0;
}
