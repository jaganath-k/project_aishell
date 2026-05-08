#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

/* Comparator state — set before each qsort call */
static int g_numeric = 0;
static int g_reverse = 0;
static int g_key     = 0;  /* 0 = whole line; N = Nth whitespace field (1-indexed) */

/* Return pointer to start of field k (1-indexed) in line, or NULL if absent. */
static const char *field_start(const char *line, int k) {
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;  /* skip leading space */
    for (int i = 1; i < k; i++) {
        while (*p && !isspace((unsigned char)*p)) p++;  /* skip field */
        while (*p &&  isspace((unsigned char)*p)) p++;  /* skip separator */
        if (!*p) return NULL;
    }
    return *p ? p : NULL;
}

static int sort_cmp(const void *a, const void *b) {
    const char *la = *(const char *const *)a;
    const char *lb = *(const char *const *)b;

    const char *fa = (g_key > 0) ? field_start(la, g_key) : la;
    const char *fb = (g_key > 0) ? field_start(lb, g_key) : lb;
    if (!fa) fa = "";
    if (!fb) fb = "";

    int cmp;
    if (g_numeric) {
        double da = strtod(fa, NULL);
        double db = strtod(fb, NULL);
        cmp = (da > db) - (da < db);
    } else {
        cmp = strcmp(fa, fb);
    }
    return g_reverse ? -cmp : cmp;
}

/* Grow a lines array by reading every line from f. */
static int append_file(FILE *f, char ***lines, int *count, int *cap) {
    char buf[8192];
    while (fgets(buf, sizeof(buf), f)) {
        if (*count == *cap) {
            *cap  *= 2;
            char **tmp = realloc(*lines, (size_t)*cap * sizeof(char *));
            if (!tmp) return -1;
            *lines = tmp;
        }
        (*lines)[(*count)++] = strdup(buf);
    }
    return 0;
}

static void build_sort_argtable(arg_lit_t  **help,
                                 arg_lit_t  **help_json,
                                 arg_lit_t  **reverse,
                                 arg_lit_t  **numeric,
                                 arg_lit_t  **unique,
                                 arg_int_t  **key,
                                 arg_file_t **files,
                                 arg_end_t  **end,
                                 void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",         "show help and exit");
    *help_json = arg_lit0(NULL, "help-json",   "print machine-readable help as JSON");
    *reverse   = arg_lit0("r", "reverse",      "reverse the sort order");
    *numeric   = arg_lit0("n", "numeric-sort", "compare by numeric value");
    *unique    = arg_lit0("u", "unique",        "output only the first of equal lines");
    *key       = arg_int0("k", "key", "N",     "sort by field N (1-indexed, whitespace-delimited)");
    *files     = arg_filen(NULL, NULL, "FILE...", 0, 64, "files to sort (default: stdin)");
    *end       = arg_end(20);

    static void *argtable[9];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *reverse;
    argtable[3] = *numeric;
    argtable[4] = *unique;
    argtable[5] = *key;
    argtable[6] = *files;
    argtable[7] = *end;
    argtable[8] = NULL;
    *argtable_out = argtable;
}

void sort_print_usage(FILE *out);
extern cmd_spec_t cmd_sort_spec;

int sort_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json, *reverse, *numeric, *unique;
    arg_int_t  *key;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_sort_argtable(&help, &help_json, &reverse, &numeric, &unique,
                        &key, &files, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 8);
        sort_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_sort_spec, argtable, stdout);
        arg_freetable(argtable, 8);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "sort");
        arg_freetable(argtable, 8);
        sort_print_usage(stdout);
        return 1;
    }

    g_reverse = (reverse->count > 0);
    g_numeric = (numeric->count > 0);
    g_key     = (key->count > 0) ? key->ival[0] : 0;
    int do_unique = (unique->count > 0);

    /* Read all input into a lines array */
    int    cap   = 256;
    int    count = 0;
    char **lines = malloc((size_t)cap * sizeof(char *));
    if (!lines) { arg_freetable(argtable, 8); return 1; }

    int ret = 0;
    if (files->count == 0) {
        if (append_file(stdin, &lines, &count, &cap) < 0) ret = 1;
    } else {
        for (int i = 0; i < files->count; i++) {
            FILE *f = fopen(files->filename[i], "r");
            if (!f) {
                fprintf(stderr, "sort: %s: %s\n", files->filename[i], strerror(errno));
                ret = 1;
                continue;
            }
            if (append_file(f, &lines, &count, &cap) < 0) ret = 1;
            fclose(f);
        }
    }

    arg_freetable(argtable, 8);

    qsort(lines, (size_t)count, sizeof(char *), sort_cmp);

    /* Print pass — free separately so the unique check never reads freed memory */
    char *last = NULL;
    for (int i = 0; i < count; i++) {
        if (do_unique && last && sort_cmp(&lines[i], &last) == 0)
            continue;
        fputs(lines[i], stdout);
        last = lines[i];
    }
    for (int i = 0; i < count; i++) free(lines[i]);
    free(lines);
    return ret;
}

void sort_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json, *reverse, *numeric, *unique;
    arg_int_t  *key;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_sort_argtable(&help, &help_json, &reverse, &numeric, &unique,
                        &key, &files, &end, &argtable);
    fprintf(out, "Usage: sort ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nSort lines of text from FILE(s) and write to standard output.\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-28s %s\n");
    arg_freetable(argtable, 8);
}

cmd_spec_t cmd_sort_spec = {
    .name      = "sort",
    .summary   = "sort lines of text files",
    .long_help = "Sort lines from FILE(s) and write sorted output to stdout. "
                 "With no FILE, read from stdin. Use -n for numeric sort, "
                 "-r to reverse, -u to suppress duplicate lines, "
                 "-k N to sort by the Nth whitespace-delimited field.",
    .run         = sort_run,
    .print_usage = sort_print_usage,
};

void register_sort_command(void) {
    register_command(&cmd_sort_spec);
}
