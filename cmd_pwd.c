#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

static void build_pwd_argtable(arg_lit_t  **help,
                                arg_lit_t  **help_json,
                                arg_end_t  **end,
                                void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json","print machine-readable help as JSON");
    *end       = arg_end(20);

    static void *argtable[4];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *end;
    argtable[3] = NULL;

    *argtable_out = argtable;
}

void pwd_print_usage(FILE *out);
extern cmd_spec_t cmd_pwd_spec;

int pwd_run(int argc, char **argv) {
    arg_lit_t *help, *help_json;
    arg_end_t *end;
    void     **argtable;

    build_pwd_argtable(&help, &help_json, &end, &argtable);

    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 3);
        pwd_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_pwd_spec, argtable, stdout);
        arg_freetable(argtable, 3);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "pwd");
        arg_freetable(argtable, 3);
        pwd_print_usage(stdout);
        return 1;
    }

    arg_freetable(argtable, 3);

    char *cwd = getcwd(NULL, 0);
    if (!cwd) {
        fprintf(stderr, "pwd: %s\n", strerror(errno));
        return 1;
    }
    puts(cwd);
    free(cwd);
    return 0;
}

void pwd_print_usage(FILE *out) {
    arg_lit_t *help, *help_json;
    arg_end_t *end;
    void     **argtable;

    build_pwd_argtable(&help, &help_json, &end, &argtable);

    fprintf(out, "Usage: pwd ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-20s %s\n");

    arg_freetable(argtable, 3);
}

cmd_spec_t cmd_pwd_spec = {
    .name      = "pwd",
    .summary   = "print name of current/working directory",
    .long_help = "Print the full filename of the current working directory.",
    .run         = pwd_run,
    .print_usage = pwd_print_usage,
};

void register_pwd_command(void) {
    register_command(&cmd_pwd_spec);
}
