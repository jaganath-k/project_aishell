#ifndef HASH_UTILS_H
#define HASH_UTILS_H

#include <stdio.h>

/* Returns a static 33-char string (32 hex + NUL). */
const char *md5_hexdigest(FILE *fp);

/* Returns a static 65-char string (64 hex + NUL). */
const char *sha256_hexdigest(FILE *fp);

#endif
