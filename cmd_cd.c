#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

static void build_cd_argtable(arg_lit_t  **help,
                               arg_lit_t  **help_json,
                               arg_str_t  **dir,
                               arg_end_t  **end,
                               void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json","print machine-readable help as JSON");
    *dir       = arg_str0(NULL, NULL,       "DIR", "directory to change to (default: $HOME)");
    *end       = arg_end(20);

    static void *argtable[5];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *dir;
    argtable[3] = *end;
    argtable[4] = NULL;

    *argtable_out = argtable;
}

void cd_print_usage(FILE *out);
extern cmd_spec_t cmd_cd_spec;

int cd_run(int argc, char **argv) {
    arg_lit_t *help, *help_json;
    arg_str_t *dir;
    arg_end_t *end;
    void     **argtable;

    build_cd_argtable(&help, &help_json, &dir, &end, &argtable);

    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 4);
        cd_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_cd_spec, argtable, stdout);
        arg_freetable(argtable, 4);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "cd");
        arg_freetable(argtable, 4);
        cd_print_usage(stdout);
        return 1;
    }

    const char *target;
    if (dir->count > 0) {
        target = dir->sval[0];
    } else {
        target = getenv("HOME");
        if (!target) {
            fprintf(stderr, "cd: HOME not set\n");
            arg_freetable(argtable, 4);
            return 1;
        }
    }

    arg_freetable(argtable, 4);

    if (chdir(target) != 0) {
        fprintf(stderr, "cd: %s: %s\n", target, strerror(errno));
        return 1;
    }
    return 0;
}

void cd_print_usage(FILE *out) {
    arg_lit_t *help, *help_json;
    arg_str_t *dir;
    arg_end_t *end;
    void     **argtable;

    build_cd_argtable(&help, &help_json, &dir, &end, &argtable);

    fprintf(out, "Usage: cd ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-20s %s\n");

    arg_freetable(argtable, 4);
}

cmd_spec_t cmd_cd_spec = {
    .name      = "cd",
    .summary   = "change the working directory",
    .long_help = "Change the current working directory to DIR. "
                 "With no argument, changes to the directory named in $HOME.",
    .run         = cd_run,
    .print_usage = cd_print_usage,
};

void register_cd_command(void) {
    register_command(&cmd_cd_spec);
}
