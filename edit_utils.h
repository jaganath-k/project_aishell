#ifndef EDIT_UTILS_H
#define EDIT_UTILS_H

#include <stdio.h>

/* Read all lines from path into a heap-allocated array.
   Each entry is a strdup'd line, including its trailing '\n'.
   Sets *count to the number of lines. Returns NULL on error.
   Caller must free with edit_free_lines(). */
char **edit_read_lines(const char *path, int *count);

/* Write lines[0..count-1] back to path.
   Uses a temp file + rename for atomicity so a write failure
   never leaves the original file truncated.
   Returns 0 on success, -1 on error (errno set). */
int edit_write_lines(const char *path, char **lines, int count);

/* Free the array returned by edit_read_lines. */
void edit_free_lines(char **lines, int count);

/* Return a heap-allocated copy of text that ends with '\n'.
   If text already ends with '\n', this is equivalent to strdup.
   Caller must free(). */
char *edit_ensure_newline(const char *text);

#endif
