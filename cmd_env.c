#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

extern char **environ;

static void build_env_argtable(arg_lit_t **help,
                                arg_lit_t **help_json,
                                arg_str_t **unsets,
                                arg_str_t **vars,
                                arg_end_t **end,
                                void      ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json","print machine-readable help as JSON");
    *unsets    = arg_strn("u", NULL, "NAME", 0, 64, "remove NAME from environment before printing");
    *vars      = arg_strn(NULL, NULL, "NAME=VALUE", 0, 64, "set variable in environment before printing");
    *end       = arg_end(20);

    static void *argtable[6];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *unsets;
    argtable[3] = *vars;
    argtable[4] = *end;
    argtable[5] = NULL;

    *argtable_out = argtable;
}

void env_print_usage(FILE *out);
extern cmd_spec_t cmd_env_spec;

int env_run(int argc, char **argv) {
    arg_lit_t *help, *help_json;
    arg_str_t *unsets, *vars;
    arg_end_t *end;
    void **argtable;

    build_env_argtable(&help, &help_json, &unsets, &vars, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 5);
        env_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_env_spec, argtable, stdout);
        arg_freetable(argtable, 5);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "env");
        arg_freetable(argtable, 5);
        env_print_usage(stdout);
        return 1;
    }

    /* Apply -u unsets */
    for (int i = 0; i < unsets->count; i++)
        unsetenv(unsets->sval[i]);

    /* Apply NAME=VALUE assignments */
    for (int i = 0; i < vars->count; i++) {
        const char *a = vars->sval[i];
        const char *eq = strchr(a, '=');
        if (!eq) {
            fprintf(stderr, "env: '%s': not a valid NAME=VALUE assignment\n", a);
            arg_freetable(argtable, 5);
            return 1;
        }
        char name[256];
        size_t nlen = (size_t)(eq - a);
        if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
        memcpy(name, a, nlen);
        name[nlen] = '\0';
        setenv(name, eq + 1, 1);
    }

    arg_freetable(argtable, 5);

    for (char **e = environ; *e; e++)
        puts(*e);

    return 0;
}

void env_print_usage(FILE *out) {
    arg_lit_t *help, *help_json;
    arg_str_t *unsets, *vars;
    arg_end_t *end;
    void **argtable;

    build_env_argtable(&help, &help_json, &unsets, &vars, &end, &argtable);

    fprintf(out, "Usage: env ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-30s %s\n");

    arg_freetable(argtable, 5);
}

cmd_spec_t cmd_env_spec = {
    .name      = "env",
    .summary   = "print environment or set variables for display",
    .long_help = "With no arguments, print all environment variables. Use -u NAME to "
                 "remove a variable before printing, and NAME=VALUE to temporarily set "
                 "a variable. Changes persist for the current shell session.",
    .run         = env_run,
    .print_usage = env_print_usage,
};

void register_env_command(void) {
    register_command(&cmd_env_spec);
}
