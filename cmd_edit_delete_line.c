#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"
#include "edit_utils.h"

static void build_edit_delete_line_argtable(
        arg_lit_t  **help,
        arg_lit_t  **help_json,
        arg_int_t  **startline,
        arg_int_t  **endline,
        arg_file_t **file,
        arg_end_t  **end,
        void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json","print machine-readable help as JSON");
    *startline = arg_int1("n", "line",  "N", "first line to delete (1-indexed)");
    *endline   = arg_int0("e", "end",   "E", "last line to delete inclusive (default: same as -n)");
    *file      = arg_filen(NULL, NULL, "FILE", 1, 1, "file to edit");
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

void edit_delete_line_print_usage(FILE *out);
extern cmd_spec_t cmd_edit_delete_line_spec;

int edit_delete_line_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json;
    arg_int_t  *startline, *endline;
    arg_file_t *file;
    arg_end_t  *end;
    void      **argtable;

    build_edit_delete_line_argtable(&help, &help_json, &startline, &endline,
                                    &file, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 6);
        edit_delete_line_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_edit_delete_line_spec, argtable, stdout);
        arg_freetable(argtable, 6);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "edit-delete-line");
        arg_freetable(argtable, 6);
        edit_delete_line_print_usage(stdout);
        return 1;
    }

    int         first = startline->ival[0];
    int         last  = (endline->count > 0) ? endline->ival[0] : first;
    const char *path  = file->filename[0];

    if (first > last) {
        fprintf(stderr, "edit-delete-line: start line %d is after end line %d\n",
                first, last);
        arg_freetable(argtable, 6);
        return 1;
    }

    int     count;
    char  **lines = edit_read_lines(path, &count);
    if (!lines) {
        fprintf(stderr, "edit-delete-line: %s: %s\n", path, strerror(errno));
        arg_freetable(argtable, 6);
        return 1;
    }

    if (first < 1 || last > count) {
        fprintf(stderr,
                "edit-delete-line: range %d-%d out of bounds (file has %d line%s)\n",
                first, last, count, count == 1 ? "" : "s");
        edit_free_lines(lines, count);
        arg_freetable(argtable, 6);
        return 1;
    }

    /* Free the lines being deleted */
    for (int i = first - 1; i < last; i++)
        free(lines[i]);

    /* Compact: shift surviving lines left to fill the gap */
    int del      = last - first + 1;
    int newcount = count - del;
    for (int i = last; i < count; i++)
        lines[i - del] = lines[i];

    int ret = 0;
    if (edit_write_lines(path, lines, newcount) < 0) {
        fprintf(stderr, "edit-delete-line: %s: %s\n", path, strerror(errno));
        ret = 1;
    }

    for (int i = 0; i < newcount; i++) free(lines[i]);
    free(lines);
    arg_freetable(argtable, 6);
    return ret;
}

void edit_delete_line_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json;
    arg_int_t  *startline, *endline;
    arg_file_t *file;
    arg_end_t  *end;
    void      **argtable;

    build_edit_delete_line_argtable(&help, &help_json, &startline, &endline,
                                    &file, &end, &argtable);
    fprintf(out, "Usage: edit-delete-line ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nDelete one line or a range of lines from FILE.\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-28s %s\n");
    arg_freetable(argtable, 6);
}

cmd_spec_t cmd_edit_delete_line_spec = {
    .name      = "edit-delete-line",
    .summary   = "delete one or more lines from a file by line number",
    .long_help = "Delete line N from FILE. With -e E, delete the range N through E "
                 "inclusive. Line numbers are 1-indexed. All subsequent lines shift up. "
                 "The file is updated atomically via a temp file and rename.",
    .run         = edit_delete_line_run,
    .print_usage = edit_delete_line_print_usage,
};

void register_edit_delete_line_command(void) {
    register_command(&cmd_edit_delete_line_spec);
}
