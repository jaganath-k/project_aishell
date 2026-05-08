#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

static void build_cat_argtable(arg_lit_t  **help,
                                arg_lit_t  **help_json,
                                arg_lit_t  **linenums,
                                arg_file_t **files,
                                arg_end_t  **end,
                                void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json", "print machine-readable help as JSON");
    *linenums  = arg_lit0("n", NULL,         "number all output lines");
    *files     = arg_filen(NULL, NULL, "FILE...", 1, 64, "files to concatenate");
    *end       = arg_end(20);

    static void *argtable[6];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *linenums;
    argtable[3] = *files;
    argtable[4] = *end;
    argtable[5] = NULL;

    *argtable_out = argtable;
}

void cat_print_usage(FILE *out);
extern cmd_spec_t cmd_cat_spec;

int cat_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json, *linenums;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_cat_argtable(&help, &help_json, &linenums, &files, &end, &argtable);

    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 5);
        cat_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_cat_spec, argtable, stdout);
        arg_freetable(argtable, 5);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "cat");
        arg_freetable(argtable, 5);
        cat_print_usage(stdout);
        return 1;
    }

    int show_linenums = (linenums->count > 0);
    int ret           = 0;

    for (int i = 0; i < files->count; i++) {
        const char *filepath = files->filename[i];
        FILE *fp = fopen(filepath, "r");

        if (fp == NULL) {
            fprintf(stderr, "cat: %s: %s\n", filepath, strerror(errno));
            ret = 1;
            continue;
        }

        int c;
        int lnum = 0;

        if (show_linenums)
            printf("%6d  ", ++lnum);

        while ((c = fgetc(fp)) != EOF) {
            fputc(c, stdout);
            if (show_linenums && c == '\n')
                printf("%6d  ", ++lnum);
        }

        if (show_linenums)
            printf("\n");

        fclose(fp);
    }

    arg_freetable(argtable, 5);
    return ret;
}

void cat_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json, *linenums;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_cat_argtable(&help, &help_json, &linenums, &files, &end, &argtable);

    fprintf(out, "Usage: cat ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-20s %s\n");

    arg_freetable(argtable, 5);
}

cmd_spec_t cmd_cat_spec = {
    .name        = "cat",
    .summary     = "concatenate files and print on stdout",
    .long_help   = "Concatenate FILE(s) to standard output. "
                   "With no FILE, or when FILE is -, read standard input.",
    .run         = cat_run,
    .print_usage = cat_print_usage,
};

void register_cat_command(void) {
    register_command(&cmd_cat_spec);
}
