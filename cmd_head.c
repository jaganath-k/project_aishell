#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

static void build_head_argtable(arg_lit_t  **help,
                                 arg_lit_t  **help_json,
                                 arg_int_t  **lines,
                                 arg_file_t **files,
                                 arg_end_t  **end,
                                 void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json", "print machine-readable help as JSON");
    *lines     = arg_int0("n", NULL,         "N", "print first N lines (default: 10)");
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

void head_print_usage(FILE *out);
extern cmd_spec_t cmd_head_spec;

int head_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json;
    arg_int_t  *lines;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_head_argtable(&help, &help_json, &lines, &files, &end, &argtable);

    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 5);
        head_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_head_spec, argtable, stdout);
        arg_freetable(argtable, 5);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "head");
        arg_freetable(argtable, 5);
        head_print_usage(stdout);
        return 1;
    }

    int lines_to_print = 10;
    if (lines->count > 0) {
        lines_to_print = lines->ival[0];
        if (lines_to_print < 0) {
            fprintf(stderr, "head: invalid number of lines: %d\n",
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
            fprintf(stderr, "head: cannot open '%s': %s\n",
                    filepath, strerror(errno));
            ret = 1;
            continue;
        }

        if (show_header)
            printf("==> %s <==\n", filepath);

        char   *line    = NULL;
        size_t  len     = 0;
        int     printed = 0;
        ssize_t nread;

        while (printed < lines_to_print &&
               (nread = getline(&line, &len, fp)) != -1) {
            fputs(line, stdout);
            printed++;
        }

        free(line);
        fclose(fp);

        if (i < file_count - 1 && show_header)
            printf("\n");
    }

    arg_freetable(argtable, 5);
    return ret;
}

void head_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json;
    arg_int_t  *lines;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_head_argtable(&help, &help_json, &lines, &files, &end, &argtable);

    fprintf(out, "Usage: head ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-20s %s\n");

    arg_freetable(argtable, 5);
}

cmd_spec_t cmd_head_spec = {
    .name        = "head",
    .summary     = "output the first part of files",
    .long_help   = "Print the first 10 lines of each FILE to standard output. "
                   "With more than one FILE, precede each with a header.",
    .run         = head_run,
    .print_usage = head_print_usage,
};

void register_head_command(void) {
    register_command(&cmd_head_spec);
}
