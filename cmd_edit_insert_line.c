#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"
#include "edit_utils.h"

static void build_edit_insert_line_argtable(
        arg_lit_t  **help,
        arg_lit_t  **help_json,
        arg_int_t  **linenum,
        arg_lit_t  **after,
        arg_str_t  **text,
        arg_file_t **file,
        arg_end_t  **end,
        void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json","print machine-readable help as JSON");
    *linenum   = arg_int1("n", "line", "N", "reference line number (1-indexed)");
    *after     = arg_lit0("a", "after",     "insert after line N (default: before line N)");
    *text      = arg_str1("t", "text", "TEXT", "text to insert");
    *file      = arg_filen(NULL, NULL, "FILE", 1, 1, "file to edit");
    *end       = arg_end(20);

    static void *argtable[8];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *linenum;
    argtable[3] = *after;
    argtable[4] = *text;
    argtable[5] = *file;
    argtable[6] = *end;
    argtable[7] = NULL;
    *argtable_out = argtable;
}

void edit_insert_line_print_usage(FILE *out);
extern cmd_spec_t cmd_edit_insert_line_spec;

int edit_insert_line_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json, *after;
    arg_int_t  *linenum;
    arg_str_t  *text;
    arg_file_t *file;
    arg_end_t  *end;
    void      **argtable;

    build_edit_insert_line_argtable(&help, &help_json, &linenum, &after,
                                    &text, &file, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 7);
        edit_insert_line_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_edit_insert_line_spec, argtable, stdout);
        arg_freetable(argtable, 7);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "edit-insert-line");
        arg_freetable(argtable, 7);
        edit_insert_line_print_usage(stdout);
        return 1;
    }

    int         n            = linenum->ival[0];
    int         insert_after = (after->count > 0);
    const char *txt          = text->sval[0];
    const char *path         = file->filename[0];

    int     count;
    char  **lines = edit_read_lines(path, &count);
    if (!lines) {
        fprintf(stderr, "edit-insert-line: %s: %s\n", path, strerror(errno));
        arg_freetable(argtable, 7);
        return 1;
    }

    if (n < 1 || n > count) {
        fprintf(stderr, "edit-insert-line: line %d out of range (file has %d line%s)\n",
                n, count, count == 1 ? "" : "s");
        edit_free_lines(lines, count);
        arg_freetable(argtable, 7);
        return 1;
    }

    char *newline = edit_ensure_newline(txt);
    if (!newline) {
        edit_free_lines(lines, count);
        arg_freetable(argtable, 7);
        return 1;
    }

    /* Allocate new array with one extra slot */
    char **newlines = malloc((size_t)(count + 1) * sizeof(char *));
    if (!newlines) {
        free(newline);
        edit_free_lines(lines, count);
        arg_freetable(argtable, 7);
        return 1;
    }

    /* insert_pos: 0-indexed position of the new line in the output */
    int insert_pos = insert_after ? n : (n - 1);

    for (int i = 0; i < insert_pos; i++)
        newlines[i] = lines[i];
    newlines[insert_pos] = newline;
    for (int i = insert_pos; i < count; i++)
        newlines[i + 1] = lines[i];

    free(lines);  /* shallow free: string pointers now owned by newlines */

    int ret = 0;
    if (edit_write_lines(path, newlines, count + 1) < 0) {
        fprintf(stderr, "edit-insert-line: %s: %s\n", path, strerror(errno));
        ret = 1;
    }

    edit_free_lines(newlines, count + 1);
    arg_freetable(argtable, 7);
    return ret;
}

void edit_insert_line_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json, *after;
    arg_int_t  *linenum;
    arg_str_t  *text;
    arg_file_t *file;
    arg_end_t  *end;
    void      **argtable;

    build_edit_insert_line_argtable(&help, &help_json, &linenum, &after,
                                    &text, &file, &end, &argtable);
    fprintf(out, "Usage: edit-insert-line ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nInsert a new line before (or after with -a) line N in FILE.\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-28s %s\n");
    arg_freetable(argtable, 7);
}

cmd_spec_t cmd_edit_insert_line_spec = {
    .name      = "edit-insert-line",
    .summary   = "insert a line into a file at a given position",
    .long_help = "Insert TEXT before line N in FILE. Use -a to insert after line N. "
                 "Line numbers are 1-indexed. All subsequent lines shift down by one. "
                 "The file is updated atomically via a temp file and rename.",
    .run         = edit_insert_line_run,
    .print_usage = edit_insert_line_print_usage,
};

void register_edit_insert_line_command(void) {
    register_command(&cmd_edit_insert_line_spec);
}
