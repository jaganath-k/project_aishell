#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

extern char **environ;

static void build_export_argtable(arg_lit_t **help,
                                   arg_lit_t **help_json,
                                   arg_str_t **vars,
                                   arg_end_t **end,
                                   void      ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json","print machine-readable help as JSON");
    *vars      = arg_strn(NULL, NULL, "NAME[=VALUE]", 0, 64,
                          "variable to export; NAME=VALUE sets it, NAME alone shows it");
    *end       = arg_end(20);

    static void *argtable[5];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *vars;
    argtable[3] = *end;
    argtable[4] = NULL;

    *argtable_out = argtable;
}

void export_print_usage(FILE *out);
extern cmd_spec_t cmd_export_spec;

int export_run(int argc, char **argv) {
    arg_lit_t *help, *help_json;
    arg_str_t *vars;
    arg_end_t *end;
    void **argtable;

    build_export_argtable(&help, &help_json, &vars, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 4);
        export_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_export_spec, argtable, stdout);
        arg_freetable(argtable, 4);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "export");
        arg_freetable(argtable, 4);
        export_print_usage(stdout);
        return 1;
    }

    /* No args: list all env vars in declare -x format */
    if (vars->count == 0) {
        arg_freetable(argtable, 4);
        for (char **e = environ; *e; e++)
            printf("declare -x %s\n", *e);
        return 0;
    }

    int ret = 0;
    for (int i = 0; i < vars->count; i++) {
        const char *a = vars->sval[i];
        const char *eq = strchr(a, '=');
        if (eq) {
            /* NAME=VALUE: set the variable */
            char name[256];
            size_t nlen = (size_t)(eq - a);
            if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
            memcpy(name, a, nlen);
            name[nlen] = '\0';
            setenv(name, eq + 1, 1);
        } else {
            /* NAME only: display current value */
            const char *val = getenv(a);
            if (val)
                printf("declare -x %s=\"%s\"\n", a, val);
            else {
                fprintf(stderr, "export: '%s': not found\n", a);
                ret = 1;
            }
        }
    }

    arg_freetable(argtable, 4);
    return ret;
}

void export_print_usage(FILE *out) {
    arg_lit_t *help, *help_json;
    arg_str_t *vars;
    arg_end_t *end;
    void **argtable;

    build_export_argtable(&help, &help_json, &vars, &end, &argtable);

    fprintf(out, "Usage: export ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-30s %s\n");

    arg_freetable(argtable, 4);
}

cmd_spec_t cmd_export_spec = {
    .name      = "export",
    .summary   = "set or display environment variables",
    .long_help = "With no arguments, list all exported variables in 'declare -x' format. "
                 "With NAME=VALUE, set the variable in the current shell environment. "
                 "With NAME only, display that variable's current value.",
    .run         = export_run,
    .print_usage = export_print_usage,
};

void register_export_command(void) {
    register_command(&cmd_export_spec);
}
