#include "caller_reference.h"

/* Authored initialized storage for extracted create_message's first word read;
 * this is not the engine allocator or a heap/clearing-policy qualification. */
void *caller_reference_allocate(long size, boolean clear, const char *file, long line)
{
    if (size < 0) {
        fprintf(stderr, "CALLER ASSERT %s:%ld\n", file, line);
        exit(2);
    }
    (void)clear;
    return calloc(1, (size_t)size);
}
