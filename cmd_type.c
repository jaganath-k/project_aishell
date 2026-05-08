#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

/* Shell built-ins handled in aishell_main.c before registry dispatch */
static const char * const shell_builtins[] = { "exit", "prompt", NULL };

static int is_shell_builtin(const char *name) {
    for (int i = 0; shell_builtins[i]; i++)
        if (strcmp(shell_builtins[i], name) == 0) return 1;
    return 0;
}

static void build_type_argtable(arg_lit_t **help,
                                 arg_lit_t **help_json,
                                 arg_str_t **names,
                                 arg_end_t **end,
                                 void      ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json","print machine-readable help as JSON");
    *names     = arg_strn(NULL, NULL, "NAME", 1, 64, "name(s) to look up");
    *end       = arg_end(20);

    static void *argtable[5];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *names;
    argtable[3] = *end;
    argtable[4] = NULL;

    *argtable_out = argtable;
}

void type_print_usage(FILE *out);
extern cmd_spec_t cmd_type_spec;

int type_run(int argc, char **argv) {
    arg_lit_t *help, *help_json;
    arg_str_t *names;
    arg_end_t *end;
    void **argtable;

    build_type_argtable(&help, &help_json, &names, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 4);
        type_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_type_spec, argtable, stdout);
        arg_freetable(argtable, 4);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "type");
        arg_freetable(argtable, 4);
        type_print_usage(stdout);
        return 1;
    }

    int ret = 0;
    for (int i = 0; i < names->count; i++) {
        const char *name = names->sval[i];
        if (is_shell_builtin(name)) {
            printf("%s is a shell builtin\n", name);
        } else {
            const cmd_spec_t *spec = find_command(name);
            if (spec)
                printf("%s is a registered command - %s\n", name, spec->summary);
            else {
                fprintf(stderr, "type: %s: not found\n", name);
                ret = 1;
            }
        }
    }

    arg_freetable(argtable, 4);
    return ret;
}

void type_print_usage(FILE *out) {
    arg_lit_t *help, *help_json;
    arg_str_t *names;
    arg_end_t *end;
    void **argtable;

    build_type_argtable(&help, &help_json, &names, &end, &argtable);

    fprintf(out, "Usage: type ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-30s %s\n");

    arg_freetable(argtable, 4);
}

cmd_spec_t cmd_type_spec = {
    .name      = "type",
    .summary   = "show how each name would be interpreted by the shell",
    .long_help = "For each NAME, indicate whether it is a shell builtin (exit, prompt) "
                 "or a registered command in the aishell registry. Registered commands "
                 "also show their one-line summary.",
    .run         = type_run,
    .print_usage = type_print_usage,
};

void register_type_command(void) {
    register_command(&cmd_type_spec);
}
