#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

static void build_wc_argtable(arg_lit_t  **help,
                               arg_lit_t  **help_json,
                               arg_lit_t  **lines,
                               arg_lit_t  **words,
                               arg_lit_t  **bytes,
                               arg_file_t **files,
                               arg_end_t  **end,
                               void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json","print machine-readable help as JSON");
    *lines     = arg_lit0("l", "lines",     "print the newline count");
    *words     = arg_lit0("w", "words",     "print the word count");
    *bytes     = arg_lit0("c", "bytes",     "print the byte count");
    *files     = arg_filen(NULL, NULL, "FILE...", 0, 64, "files to count (default: stdin)");
    *end       = arg_end(20);

    static void *argtable[8];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *lines;
    argtable[3] = *words;
    argtable[4] = *bytes;
    argtable[5] = *files;
    argtable[6] = *end;
    argtable[7] = NULL;
    *argtable_out = argtable;
}

/* Count lines, words, bytes in one open stream. */
static void count_stream(FILE *f, long *out_lines,
                          long *out_words, long *out_bytes) {
    long l = 0, w = 0, b = 0;
    int c, in_word = 0;
    while ((c = fgetc(f)) != EOF) {
        b++;
        if (c == '\n') l++;
        if (isspace((unsigned char)c)) {
            in_word = 0;
        } else if (!in_word) {
            in_word = 1;
            w++;
        }
    }
    *out_lines = l;
    *out_words = w;
    *out_bytes = b;
}

static void print_counts(long l, long w, long b,
                          int show_l, int show_w, int show_c,
                          const char *label) {
    if (show_l) printf(" %7ld", l);
    if (show_w) printf(" %7ld", w);
    if (show_c) printf(" %7ld", b);
    if (label)  printf(" %s", label);
    putchar('\n');
}

void wc_print_usage(FILE *out);
extern cmd_spec_t cmd_wc_spec;

int wc_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json, *lines, *words, *bytes;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_wc_argtable(&help, &help_json, &lines, &words, &bytes,
                      &files, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 7);
        wc_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_wc_spec, argtable, stdout);
        arg_freetable(argtable, 7);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "wc");
        arg_freetable(argtable, 7);
        wc_print_usage(stdout);
        return 1;
    }

    /* If no flags given, show all three columns */
    int show_l = (lines->count > 0 || (lines->count == 0 && words->count == 0 && bytes->count == 0));
    int show_w = (words->count > 0 || (lines->count == 0 && words->count == 0 && bytes->count == 0));
    int show_c = (bytes->count > 0 || (lines->count == 0 && words->count == 0 && bytes->count == 0));

    long total_l = 0, total_w = 0, total_b = 0;
    int  ret     = 0;

    if (files->count == 0) {
        /* stdin */
        long l, w, b;
        count_stream(stdin, &l, &w, &b);
        print_counts(l, w, b, show_l, show_w, show_c, NULL);
    } else {
        for (int i = 0; i < files->count; i++) {
            const char *path = files->filename[i];
            FILE *f = fopen(path, "r");
            if (!f) {
                fprintf(stderr, "wc: %s: %s\n", path, strerror(errno));
                ret = 1;
                continue;
            }
            long l, w, b;
            count_stream(f, &l, &w, &b);
            fclose(f);
            print_counts(l, w, b, show_l, show_w, show_c, path);
            total_l += l; total_w += w; total_b += b;
        }
        if (files->count > 1)
            print_counts(total_l, total_w, total_b, show_l, show_w, show_c, "total");
    }

    arg_freetable(argtable, 7);
    return ret;
}

void wc_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json, *lines, *words, *bytes;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_wc_argtable(&help, &help_json, &lines, &words, &bytes,
                      &files, &end, &argtable);
    fprintf(out, "Usage: wc ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nPrint line, word, and byte counts for each FILE.\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-25s %s\n");
    arg_freetable(argtable, 7);
}

cmd_spec_t cmd_wc_spec = {
    .name      = "wc",
    .summary   = "print line, word, and byte counts for each file",
    .long_help = "Print newline, word, and byte counts for each FILE. "
                 "With no FILE, read from standard input. "
                 "With no flags, print all three counts. "
                 "With multiple files, print a total line at the end.",
    .run         = wc_run,
    .print_usage = wc_print_usage,
};

void register_wc_command(void) {
    register_command(&cmd_wc_spec);
}
