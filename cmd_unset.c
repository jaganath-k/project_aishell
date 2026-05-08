#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

static void build_unset_argtable(arg_lit_t **help,
                                  arg_lit_t **help_json,
                                  arg_str_t **names,
                                  arg_end_t **end,
                                  void      ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json","print machine-readable help as JSON");
    *names     = arg_strn(NULL, NULL, "NAME", 1, 64, "variable(s) to remove from environment");
    *end       = arg_end(20);

    static void *argtable[5];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *names;
    argtable[3] = *end;
    argtable[4] = NULL;

    *argtable_out = argtable;
}

void unset_print_usage(FILE *out);
extern cmd_spec_t cmd_unset_spec;

int unset_run(int argc, char **argv) {
    arg_lit_t *help, *help_json;
    arg_str_t *names;
    arg_end_t *end;
    void **argtable;

    build_unset_argtable(&help, &help_json, &names, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 4);
        unset_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_unset_spec, argtable, stdout);
        arg_freetable(argtable, 4);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "unset");
        arg_freetable(argtable, 4);
        unset_print_usage(stdout);
        return 1;
    }

    for (int i = 0; i < names->count; i++)
        unsetenv(names->sval[i]);

    arg_freetable(argtable, 4);
    return 0;
}

void unset_print_usage(FILE *out) {
    arg_lit_t *help, *help_json;
    arg_str_t *names;
    arg_end_t *end;
    void **argtable;

    build_unset_argtable(&help, &help_json, &names, &end, &argtable);

    fprintf(out, "Usage: unset ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-30s %s\n");

    arg_freetable(argtable, 4);
}

cmd_spec_t cmd_unset_spec = {
    .name      = "unset",
    .summary   = "remove variables from the environment",
    .long_help = "Remove each NAME from the current shell environment. "
                 "Silently ignores variables that are not set.",
    .run         = unset_run,
    .print_usage = unset_print_usage,
};

void register_unset_command(void) {
    register_command(&cmd_unset_spec);
}
