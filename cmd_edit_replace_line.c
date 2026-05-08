#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"
#include "edit_utils.h"

static void build_edit_replace_line_argtable(
        arg_lit_t  **help,
        arg_lit_t  **help_json,
        arg_int_t  **linenum,
        arg_str_t  **text,
        arg_file_t **file,
        arg_end_t  **end,
        void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json","print machine-readable help as JSON");
    *linenum   = arg_int1("n", "line", "N", "line number to replace (1-indexed)");
    *text      = arg_str1("t", "text", "TEXT", "replacement text for the line");
    *file      = arg_filen(NULL, NULL, "FILE", 1, 1, "file to edit");
    *end       = arg_end(20);

    static void *argtable[7];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *linenum;
    argtable[3] = *text;
    argtable[4] = *file;
    argtable[5] = *end;
    argtable[6] = NULL;
    *argtable_out = argtable;
}

void edit_replace_line_print_usage(FILE *out);
extern cmd_spec_t cmd_edit_replace_line_spec;

int edit_replace_line_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json;
    arg_int_t  *linenum;
    arg_str_t  *text;
    arg_file_t *file;
    arg_end_t  *end;
    void      **argtable;

    build_edit_replace_line_argtable(&help, &help_json, &linenum, &text, &file, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 6);
        edit_replace_line_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_edit_replace_line_spec, argtable, stdout);
        arg_freetable(argtable, 6);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "edit-replace-line");
        arg_freetable(argtable, 6);
        edit_replace_line_print_usage(stdout);
        return 1;
    }

    int         n    = linenum->ival[0];
    const char *txt  = text->sval[0];
    const char *path = file->filename[0];

    int     count;
    char  **lines = edit_read_lines(path, &count);
    if (!lines) {
        fprintf(stderr, "edit-replace-line: %s: %s\n", path, strerror(errno));
        arg_freetable(argtable, 6);
        return 1;
    }

    if (n < 1 || n > count) {
        fprintf(stderr, "edit-replace-line: line %d out of range (file has %d line%s)\n",
                n, count, count == 1 ? "" : "s");
        edit_free_lines(lines, count);
        arg_freetable(argtable, 6);
        return 1;
    }

    char *newline = edit_ensure_newline(txt);
    if (!newline) {
        edit_free_lines(lines, count);
        arg_freetable(argtable, 6);
        return 1;
    }
    free(lines[n - 1]);
    lines[n - 1] = newline;

    int ret = 0;
    if (edit_write_lines(path, lines, count) < 0) {
        fprintf(stderr, "edit-replace-line: %s: %s\n", path, strerror(errno));
        ret = 1;
    }

    edit_free_lines(lines, count);
    arg_freetable(argtable, 6);
    return ret;
}

void edit_replace_line_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json;
    arg_int_t  *linenum;
    arg_str_t  *text;
    arg_file_t *file;
    arg_end_t  *end;
    void      **argtable;

    build_edit_replace_line_argtable(&help, &help_json, &linenum, &text, &file, &end, &argtable);
    fprintf(out, "Usage: edit-replace-line ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nReplace the entire content of a single line in FILE.\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-28s %s\n");
    arg_freetable(argtable, 6);
}

cmd_spec_t cmd_edit_replace_line_spec = {
    .name      = "edit-replace-line",
    .summary   = "replace a line in a file by line number",
    .long_help = "Replace the entire content of line N in FILE with TEXT. "
                 "Line numbers are 1-indexed. The file is updated atomically "
                 "via a temp file and rename.",
    .run         = edit_replace_line_run,
    .print_usage = edit_replace_line_print_usage,
};

void register_edit_replace_line_command(void) {
    register_command(&cmd_edit_replace_line_spec);
}
