#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"
#include "edit_utils.h"

static void build_edit_show_argtable(arg_lit_t  **help,
                                      arg_lit_t  **help_json,
                                      arg_int_t  **startline,
                                      arg_int_t  **endline,
                                      arg_file_t **file,
                                      arg_end_t  **end,
                                      void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json","print machine-readable help as JSON");
    *startline = arg_int0("n", "line",  "N", "first line to display (default: 1)");
    *endline   = arg_int0("e", "end",   "E", "last line to display (default: last line)");
    *file      = arg_filen(NULL, NULL, "FILE", 1, 1, "file to display");
    *end       = arg_end(20);

    static void *argtable[7];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *startline;
    argtable[3] = *endline;
    argtable[4] = *file;
    argtable[5] = *end;
    argtable[6] = NULL;
    *argtable_out = argtable;
}

void edit_show_print_usage(FILE *out);
extern cmd_spec_t cmd_edit_show_spec;

int edit_show_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json;
    arg_int_t  *startline, *endline;
    arg_file_t *file;
    arg_end_t  *end;
    void      **argtable;

    build_edit_show_argtable(&help, &help_json, &startline, &endline,
                              &file, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 6);
        edit_show_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_edit_show_spec, argtable, stdout);
        arg_freetable(argtable, 6);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "edit-show");
        arg_freetable(argtable, 6);
        edit_show_print_usage(stdout);
        return 1;
    }

    const char *path  = file->filename[0];
    int         first = (startline->count > 0) ? startline->ival[0] : 1;
    int         last  = (endline->count   > 0) ? endline->ival[0]   : -1; /* -1 = EOF */

    int     count;
    char  **lines = edit_read_lines(path, &count);
    if (!lines) {
        fprintf(stderr, "edit-show: %s: %s\n", path, strerror(errno));
        arg_freetable(argtable, 6);
        return 1;
    }

    if (last == -1) last = count;

    if (first < 1) first = 1;
    if (last  > count) last = count;

    if (first > last) {
        fprintf(stderr, "edit-show: line %d out of range (file has %d line%s)\n",
                first, count, count == 1 ? "" : "s");
        edit_free_lines(lines, count);
        arg_freetable(argtable, 6);
        return 1;
    }

    /* Width of the line-number column: enough digits for the last line shown */
    int width = 1;
    for (int n = last; n >= 10; n /= 10) width++;

    for (int i = first - 1; i < last; i++) {
        /* Strip trailing newline for clean printf, then re-add */
        size_t len = strlen(lines[i]);
        int    has_nl = (len > 0 && lines[i][len - 1] == '\n');
        if (has_nl) lines[i][len - 1] = '\0';
        printf("%*d  %s\n", width, i + 1, lines[i]);
    }

    edit_free_lines(lines, count);
    arg_freetable(argtable, 6);
    return 0;
}

void edit_show_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json;
    arg_int_t  *startline, *endline;
    arg_file_t *file;
    arg_end_t  *end;
    void      **argtable;

    build_edit_show_argtable(&help, &help_json, &startline, &endline,
                              &file, &end, &argtable);
    fprintf(out, "Usage: edit-show ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nDisplay FILE with line numbers. "
                 "Use -n and -e to view a specific range.\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-28s %s\n");
    arg_freetable(argtable, 6);
}

cmd_spec_t cmd_edit_show_spec = {
    .name      = "edit-show",
    .summary   = "display a file with line numbers (optionally a range)",
    .long_help = "Print FILE to stdout with line numbers. "
                 "With no options, show all lines. "
                 "Use -n N to start at line N and -e E to end at line E. "
                 "Useful for previewing a file before using the other edit-* commands.",
    .run         = edit_show_run,
    .print_usage = edit_show_print_usage,
};

void register_edit_show_command(void) {
    register_command(&cmd_edit_show_spec);
}
