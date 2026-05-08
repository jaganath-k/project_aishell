#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

static void build_echo_argtable(arg_lit_t  **help,
                                 arg_lit_t  **help_json,
                                 arg_lit_t  **no_newline,
                                 arg_str_t  **strings,
                                 arg_end_t  **end,
                                 void       ***argtable_out)
{
    *help       = arg_lit0("h", "help",      "show help and exit");
    *help_json  = arg_lit0(NULL, "help-json","print machine-readable help as JSON");
    *no_newline = arg_lit0("n", NULL,        "do not output the trailing newline");
    *strings    = arg_strn(NULL, NULL,       "STRING", 0, 64, "strings to print");
    *end        = arg_end(20);

    static void *argtable[6];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *no_newline;
    argtable[3] = *strings;
    argtable[4] = *end;
    argtable[5] = NULL;

    *argtable_out = argtable;
}

void echo_print_usage(FILE *out);
extern cmd_spec_t cmd_echo_spec;

int echo_run(int argc, char **argv) {
    arg_lit_t *help, *help_json, *no_newline;
    arg_str_t *strings;
    arg_end_t *end;
    void     **argtable;

    build_echo_argtable(&help, &help_json, &no_newline, &strings, &end, &argtable);

    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 5);
        echo_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_echo_spec, argtable, stdout);
        arg_freetable(argtable, 5);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "echo");
        arg_freetable(argtable, 5);
        echo_print_usage(stdout);
        return 1;
    }

    int suppress_newline = (no_newline->count > 0);
    int count = strings->count;

    for (int i = 0; i < count; i++) {
        if (i > 0) putchar(' ');
        fputs(strings->sval[i], stdout);
    }

    arg_freetable(argtable, 5);

    if (!suppress_newline) putchar('\n');
    return 0;
}

void echo_print_usage(FILE *out) {
    arg_lit_t *help, *help_json, *no_newline;
    arg_str_t *strings;
    arg_end_t *end;
    void     **argtable;

    build_echo_argtable(&help, &help_json, &no_newline, &strings, &end, &argtable);

    fprintf(out, "Usage: echo ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-20s %s\n");

    arg_freetable(argtable, 5);
}

cmd_spec_t cmd_echo_spec = {
    .name      = "echo",
    .summary   = "display a line of text",
    .long_help = "Print each STRING to standard output, separated by spaces and "
                 "followed by a newline. Use -n to suppress the trailing newline.",
    .run         = echo_run,
    .print_usage = echo_print_usage,
};

void register_echo_command(void) {
    register_command(&cmd_echo_spec);
}
