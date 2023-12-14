#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>


#define die(msg) do { perror(msg); exit(EXIT_FAILURE); } while (0)

// To filter out files that were already present and read for last
// Christmas, we have to know the timestamp of Christmas. We derive
// that timestamp from the current time.
time_t christmas_day(int delta_year) {
    // Get the current UNIX time
    time_t now = time(NULL);

    // Break down the timestamp (according to localtime) into days,
    // months, years with localtime_r. We use localtime_r, because
    // nobody should use glibc functions that have an internal state
    // and return us a pointer to it!
    struct tm tm;
    if (localtime_r(&now, &tm) == NULL)
        die("localtime_r");

    // Day, month, year
    if (tm.tm_mday > 24 && tm.tm_mon == 11)
        delta_year += 1; // The days after christmas
    tm.tm_mday = 24;
    tm.tm_mon  = 11;
    tm.tm_year += delta_year;

    // Time: Mid of day
    tm.tm_hour = 12;
    tm.tm_min  = 0;
    tm.tm_sec  = 0;

    // Derive DST information from local timezone
    tm.tm_isdst = -1;

    // Create an UNIX timestamp
    return mktime(&tm);
}

// For us, a letter consists of a timestamp and a pointer to a
// filename.
struct letter {
    time_t timestamp;
    char  *filename;
};


// For qsort(3), we require a compare function that compares two
// letters. We compare those letters according to their timestamp.
// Oldest first.
int letter_compare(const void* _A, const void *_B) {
    const struct letter *A = _A;
    const struct letter *B = _B;

    if (A->timestamp < B->timestamp)
        return -1;
    if (A->timestamp > B->timestamp)
        return 1;
    return 0;
}


int main(int argc, char *argv[]) {
    // If no argument was given, we crawl the current directory for
    // letters. Please note that it is _NOT_ necessary to recursively
    // crawl the directory.
    char *dirname = argc > 1 ? argv[1] : ".";

    // We open the directory with open. As it is a directory, we have
    // to specify O_DIRECTORY to let the open succeed. Thereby, Linux
    // ensures that you not accidentally open a directory as a
    // regular file. This is a difference to BSD systems, there
    // opening (or even using cat) on a directory just works.
    int dirfd = open(dirname, O_RDONLY | O_DIRECTORY);
    if (dirfd == -1)
        die("open");

    // We use growable array, by an element counter, an capacity
    // counter, and an pointer to an heap allocated piece of memory.
    struct letter *letters = NULL;
    int nletters = 0;
    int capacity = 0;


    // We calculate UNIX timestamps for the next and the previous
    // Christmas.
    time_t last_christmas = christmas_day(-1);
    time_t this_christmas = christmas_day(0);

    for (;;) {
        // We use getdents64(2) to read directory entries from the
        // directory file descriptor. The structure is explained in
        // getdents64(2).
        char buf[4096];
        ssize_t len = getdents64(dirfd, buf, sizeof(buf));
        if (len == -1) die("getdents");
        if (len == 0) // EOF
            break;

        // We iterate over all directory entries. Please note that
        // those entries are not of fixed size, but each entry is
        // d->d_reclen bytes long. Therefore, we require a special
        // iteration technique.
        struct dirent64 *d;
        for (long bpos = 0; bpos < len; bpos += d->d_reclen) {
            d = (struct dirent64 *) (buf + bpos);

            // We skip everything that is not an regular file.
            if (d->d_type != DT_REG)
                continue;

            // We use statx(2) to get the birth time of the file.
            // Here are two things to notice:
            //
            // 1. We use the directory file descriptor instead of
            //    giving a regular path. This is great as it avoids
            //    the time-of-check-time-of-use (TOCTOU) problem that
            //    would arise otherwise.
            // 2. We specifically request the BTIME from the kernel.
            struct statx s;
            int rc = statx(dirfd, d->d_name, 0, STATX_BTIME, &s);
            if (rc < 0)
                die("statx");

            // The kernel also gives us the information whether the
            // queries stat entry contains a btime or not. Because not
            // all file systems support recording an birth time. For
            // example, the /proc file system does not return birth
            // times here (try it!)
            if (!(s.stx_mask & STATX_BTIME))
                die("statx: file system does not support btime");

            // We skip all files that were born before the last or
            // after the next Christmas
            if (!(last_christmas < s.stx_btime.tv_sec
                  && s.stx_btime.tv_sec < this_christmas))
                continue;

            // Grow the dynamic array if necessary.
            if (capacity < (nletters + 1)) {
                letters = reallocarray(letters,
                                       nletters + capacity + 1,
                                       sizeof(struct letter));
                if (!letters) die("realloc");
                capacity += nletters + 1;
            }

            // Insert the letter into our letter array. For this, we
            // _have_ to use strdup for the filename as the filename
            // is _within_ our own `buf', which we will overwrite
            // with the next getdents64(2) call.
            letters[nletters].timestamp = s.stx_btime.tv_sec;
            letters[nletters].filename  = strdup(d->d_name);
            if (!letters[nletters].filename)
                die("strdup");
            nletters ++;
        }
    }

    // Sort the letters. This is easy. qsort(3) will sort our letters in-place.
    qsort(letters, nletters, sizeof(struct letter),
          letter_compare);


    // Print out the letters according to the desired format.
    for (unsigned int i = 0; i < nletters; i++) {
        printf("%ld days: %s\n",
               (this_christmas - letters[i].timestamp) / (24 * 3600),
               letters[i].filename);
    }
}
