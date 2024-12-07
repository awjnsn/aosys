#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

void cat(int fd)
{
    char buf[4096];
    ssize_t bytes_read;
    while ((bytes_read = read(fd, buf, 4096)))
    {
        if (bytes_read < 0)
        {
            perror("Read failed!");
        }
        else
        {
            ssize_t bytes_written = write(1, buf, bytes_read);
            if (bytes_written < 0)
            {
                perror("Failed to write bytes");
            }
            else if (bytes_written != bytes_read)
            {
                perror("Failed to write all read bytes");
            }
        }
    }
}

int main(int argc, char *argv[])
{
    // For cat, we have to iterate over all command-line arguments of
    // our process, where argv[0] is our program binary itself ("./cat").
    for (int idx = 1; idx < argc; idx++)
    {
        // printf("argv[%d] = %s\n", idx, argv[idx]);
        int fd = open(argv[idx], O_RDONLY);
        if (fd > 0)
        {
            cat(fd);
            if (close(fd) < 0)
            {
                perror("Failed to close file");
            }
        }
        else
        {
            perror("Failed to open file");
        }
    }

    return 0;
}
