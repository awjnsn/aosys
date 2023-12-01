#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

#define BUFFER_SIZE 4096
char buffer[BUFFER_SIZE];

int main(int argc, char *argv[]) {
    // For cat, we have to iterate over all command-line arguments of
    // our process, where argv[0] is our program binary itself ("./cat").
    for (int idx = 1; idx < argc; idx++) {
        // We open the given file name in read-only mode. Thereby, we
        // correctly work on read-only files. As a result we either
        // get a file descriptor (>= 0) or -1 if the open failed. This
        // information can also be found in the RETURN VALUE section
        // of open(2).
        int fd = open(argv[idx], O_RDONLY);
        if (fd == -1) {
            perror("open");
            return -1;
        }

        // We always read one buffer full of data from our current
        // input file. We then use write to dump this buffer to the
        // stdout file descriptor (== 1).
        while(1) {
            // A read might return less than 4096 bytes. For example,
            // at the end of the file.
            int bytes = read(fd, buffer, BUFFER_SIZE);
            if (bytes < 0) { // An error happened
                perror("read");
                return -1;
            } else if (bytes == 0) // We reached the end of the file.
                break;

            // We use write to dump out the buffer to stdout. As write
            // is allowed to write not all bytes that we ordered it to
            // write (short write), we need a loop until the buffer is
            // empty again. The pointer p always points to the next
            // character to write.
            char *p = &buffer[0];
            while(bytes > 0) {
                int ret = write(1, p, bytes);
                if (ret < 0) { // write failes
                    perror("write");
                    return -1;
                }
                p += ret;
                bytes -= ret;
            }
        }
        // As we are good citizens, we close the file descriptor
        // again. If we forget this, it might be the case that we ran
        // out of space in fdtable.
        // We do not expect close(2) to fail.
        close(fd);
    }

    return 0;
}
