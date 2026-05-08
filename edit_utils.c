#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit_utils.h"

char **edit_read_lines(const char *path, int *count) {
    *count = 0;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    int    cap   = 256;
    char **lines = malloc((size_t)cap * sizeof(char *));
    if (!lines) { fclose(f); return NULL; }

    char buf[8192];
    int  n = 0;
    while (fgets(buf, sizeof(buf), f)) {
        if (n == cap) {
            cap *= 2;
            char **tmp = realloc(lines, (size_t)cap * sizeof(char *));
            if (!tmp) { edit_free_lines(lines, n); fclose(f); return NULL; }
            lines = tmp;
        }
        lines[n] = strdup(buf);
        if (!lines[n]) { edit_free_lines(lines, n); fclose(f); return NULL; }
        n++;
    }
    fclose(f);
    *count = n;
    return lines;
}

int edit_write_lines(const char *path, char **lines, int count) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "w");
    if (!f) return -1;

    for (int i = 0; i < count; i++)
        fputs(lines[i], f);
    fclose(f);

    if (rename(tmp, path) < 0) { remove(tmp); return -1; }
    return 0;
}

void edit_free_lines(char **lines, int count) {
    if (!lines) return;
    for (int i = 0; i < count; i++) free(lines[i]);
    free(lines);
}

char *edit_ensure_newline(const char *text) {
    size_t len = strlen(text);
    if (len > 0 && text[len - 1] == '\n') return strdup(text);
    char *s = malloc(len + 2);
    if (!s) return NULL;
    memcpy(s, text, len);
    s[len]     = '\n';
    s[len + 1] = '\0';
    return s;
}
