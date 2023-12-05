#include <stdio.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <stdbool.h>
#include <unistd.h>

/* With each inotify event, the kernel supplies us with a bit mask
 * that indicates the cause of the event. With the following table,
 * with one flag per line, you can decode the mask of events.

 If you wonder about the wild syntax of the definition here, a word of
 explanation: Each definition has the form of

    TYPE VARNAME (= INITIALIZER);

 Usually, we use named typed for TYPE (e.g., int, struct bar, long).
 However, C also allows to use unnamed types that are declared in
 place. So in the following

   TYPE        = 'struct { .... } []'
   VARNAME     = inotify_event_flags
   INITIALIZER = { { IN_ACCESS, ...}, ...}
 */
struct {
    int mask;
    char *name;
} inotify_event_flags[] = {
    {IN_ACCESS, "access"},
    {IN_ATTRIB, "attrib"},
    {IN_CLOSE_WRITE, "close_write"},
    {IN_CLOSE_NOWRITE, "close_nowrite"},
    {IN_CREATE, "create"},
    {IN_DELETE, "delete"},
    {IN_DELETE_SELF, "delete_self"},
    {IN_MODIFY, "modify"},
    {IN_MOVE_SELF, "move_self"},
    {IN_MOVED_FROM, "move_from"},
    {IN_MOVED_TO, "moved_to"},
    {IN_OPEN, "open"},
    {IN_MOVE, "move"},
    {IN_CLOSE, "close"},
    {IN_MASK_ADD, "mask_add"},
    {IN_IGNORED, "ignored"},
    {IN_ISDIR, "directory"},
    {IN_UNMOUNT, "unmount"},
};

// We already know this macro from yesterday.
#define ARRAY_SIZE(arr) (sizeof(arr)/sizeof(*(arr)))

int main(void) {
    // We allocate a buffer to hold the inotify events, which are
    // variable in size.
    void *buffer = malloc(4096);
    if (!buffer) return -1;

    int inotify_fd, watch_fd;
    // First, we create a new in-kernel inotify object. As a result,
    // we get a file descriptor as a handle for that event. Usually,
    // this should not fail, but better safe than sorry.
    inotify_fd = inotify_init();
    if (inotify_fd == -1) {
        perror("inotify_init");
        return -1;
    }

    // We watch the current working directory ('.') and we listen on
    // all OPEN, ACCESS and CLOSE events. This does not imply that the
    // retrieved events do not have more flags set, but that they have
    // at least one of the given bits set.
    watch_fd = inotify_add_watch(inotify_fd, ".",
                                 IN_OPEN | IN_ACCESS | IN_CLOSE);
    if (watch_fd == -1) {
        perror("inotify_add_watch");
        return -1;
    }


    // OK, now we set up everything and we can start reading events
    // from the inotify object.
    while (true) {
        // As the man page states, we have to use read to wait for new
        // events (blocking) and retrieve them from the kernel. Here
        // it is important that we use a buffer that is larger than
        // `struct inotify_event` as a single event is usually larger
        // as it also includes the filename as a variable-length
        // struct field at the end.
        int length = read(inotify_fd, buffer, 4096);
        if (length < 0) {
            perror("inotify/read");
            break;
        }

        // At this point, buffer *can* contain multiple inotify
        // events. Therefore, we have to loop over the buffer.
        // However, as our entries are of variable length, we cannot
        // use regular array-based pointer arithmetic.
        for (
            // Init: We start with an event pointer pointing to buffer[0]
            struct inotify_event *event = buffer;
            // Cond: As long as the event-pointer points into our buffer, we are still iterating.
            (void*)event < buffer + length;
            // Step: After the loop, we increase the event-pointer by the size of the inotify_event PLUS the length of the following filename)
            event = ((void*) event) + sizeof(struct inotify_event) + event->len) {

            // Now that event points to an event, we print one line for each event:
            if (event->len)
                printf("./%s [", event->name); // <- The filename
            else
                printf(". ["); // The watched directory itself

            // We decode the mask field, by iterating over the
            // flag--string table and check if the specified bit is
            // set in the event->mask. delim is used to properly
            // insert commas.
            char *delim = "";
            for (unsigned int i = 0; i < ARRAY_SIZE(inotify_event_flags); i++) {
                if (event->mask & inotify_event_flags[i].mask) {
                    printf("%s%s", delim,
                           inotify_event_flags[i].name);
                    delim = ",";
                }
            }
            printf("]\n");
        }
    }
    // As we are nice, we free the buffer again.
    free(buffer);
    return 0;
}
