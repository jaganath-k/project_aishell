#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

static void build_tail_argtable(arg_lit_t  **help,
                                 arg_lit_t  **help_json,
                                 arg_int_t  **lines,
                                 arg_file_t **files,
                                 arg_end_t  **end,
                                 void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json", "print machine-readable help as JSON");
    *lines     = arg_int0("n", NULL,         "N", "print last N lines (default: 10)");
    *files     = arg_filen(NULL, NULL, "FILE...", 1, 64, "files to process");
    *end       = arg_end(20);

    static void *argtable[6];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *lines;
    argtable[3] = *files;
    argtable[4] = *end;
    argtable[5] = NULL;

    *argtable_out = argtable;
}

void tail_print_usage(FILE *out);
extern cmd_spec_t cmd_tail_spec;

/* Print the last `n` lines of `fp` using a circular buffer. */
static int print_tail(FILE *fp, const char *filepath, int n) {
    if (n <= 0)
        return 0;

    /* Allocate circular buffer of n line slots. */
    char **buf = calloc(n, sizeof(char *));
    if (!buf) {
        fprintf(stderr, "tail: out of memory\n");
        return 1;
    }

    int   slot  = 0;   /* next write position */
    int   count = 0;   /* total lines read    */
    char *line  = NULL;
    size_t len  = 0;

    while (getline(&line, &len, fp) != -1) {
        free(buf[slot % n]);
        buf[slot % n] = strdup(line);
        slot++;
        count++;
    }
    free(line);

    /* Print stored lines in chronological order. */
    int stored = count < n ? count : n;
    int start  = count < n ? 0 : slot % n;

    for (int i = 0; i < stored; i++) {
        int idx = (start + i) % n;
        if (buf[idx])
            fputs(buf[idx], stdout);
    }

    for (int i = 0; i < n; i++)
        free(buf[i]);
    free(buf);

    if (ferror(fp)) {
        fprintf(stderr, "tail: error reading '%s': %s\n",
                filepath, strerror(errno));
        return 1;
    }
    return 0;
}

int tail_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json;
    arg_int_t  *lines;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_tail_argtable(&help, &help_json, &lines, &files, &end, &argtable);

    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 5);
        tail_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_tail_spec, argtable, stdout);
        arg_freetable(argtable, 5);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "tail");
        arg_freetable(argtable, 5);
        tail_print_usage(stdout);
        return 1;
    }

    int lines_to_print = 10;
    if (lines->count > 0) {
        lines_to_print = lines->ival[0];
        if (lines_to_print < 0) {
            fprintf(stderr, "tail: invalid number of lines: %d\n",
                    lines_to_print);
            arg_freetable(argtable, 5);
            return 1;
        }
    }

    int file_count  = files->count;
    int show_header = (file_count > 1);
    int ret         = 0;

    for (int i = 0; i < file_count; i++) {
        const char *filepath = files->filename[i];
        FILE *fp = fopen(filepath, "r");

        if (fp == NULL) {
            fprintf(stderr, "tail: cannot open '%s': %s\n",
                    filepath, strerror(errno));
            ret = 1;
            continue;
        }

        if (show_header)
            printf("==> %s <==\n", filepath);

        if (print_tail(fp, filepath, lines_to_print) != 0)
            ret = 1;

        fclose(fp);

        if (i < file_count - 1 && show_header)
            printf("\n");
    }

    arg_freetable(argtable, 5);
    return ret;
}

void tail_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json;
    arg_int_t  *lines;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_tail_argtable(&help, &help_json, &lines, &files, &end, &argtable);

    fprintf(out, "Usage: tail ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-20s %s\n");

    arg_freetable(argtable, 5);
}

cmd_spec_t cmd_tail_spec = {
    .name      = "tail",
    .summary   = "output the last part of files",
    .long_help = "Print the last 10 lines of each FILE to standard output. "
                 "With more than one FILE, precede each with a header.",
    .run         = tail_run,
    .print_usage = tail_print_usage,
};

void register_tail_command(void) {
    register_command(&cmd_tail_spec);
}
