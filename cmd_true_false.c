#include <stdio.h>
#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

/* ── Shared argtable builder (help + help-json only, no other args) ───── */

static void build_tf_argtable(arg_lit_t **help,
                               arg_lit_t **help_json,
                               arg_end_t **end,
                               void      ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json", "print machine-readable help as JSON");
    *end       = arg_end(20);

    static void *argtable[4];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *end;
    argtable[3] = NULL;
    *argtable_out = argtable;
}

/* ── true ─────────────────────────────────────────────────────────────── */

void true_print_usage(FILE *out);
extern cmd_spec_t cmd_true_spec;

int true_run(int argc, char **argv) {
    arg_lit_t *help, *help_json;
    arg_end_t *end;
    void     **argtable;

    build_tf_argtable(&help, &help_json, &end, &argtable);
    arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 3);
        true_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_true_spec, argtable, stdout);
        arg_freetable(argtable, 3);
        return 0;
    }

    arg_freetable(argtable, 3);
    return 0;
}

void true_print_usage(FILE *out) {
    arg_lit_t *help, *help_json;
    arg_end_t *end;
    void     **argtable;

    build_tf_argtable(&help, &help_json, &end, &argtable);
    fprintf(out, "Usage: true ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nDo nothing, successfully (exit code 0).\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-28s %s\n");
    arg_freetable(argtable, 3);
}

cmd_spec_t cmd_true_spec = {
    .name      = "true",
    .summary   = "do nothing, successfully",
    .long_help = "Exit with a status code of 0 (success). "
                 "Useful as a loop condition: while true; do ...; done",
    .run         = true_run,
    .print_usage = true_print_usage,
};

void register_true_command(void) {
    register_command(&cmd_true_spec);
}

/* ── false ────────────────────────────────────────────────────────────── */

void false_print_usage(FILE *out);
extern cmd_spec_t cmd_false_spec;

int false_run(int argc, char **argv) {
    arg_lit_t *help, *help_json;
    arg_end_t *end;
    void     **argtable;

    build_tf_argtable(&help, &help_json, &end, &argtable);
    arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 3);
        false_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_false_spec, argtable, stdout);
        arg_freetable(argtable, 3);
        return 0;
    }

    arg_freetable(argtable, 3);
    return 1;
}

void false_print_usage(FILE *out) {
    arg_lit_t *help, *help_json;
    arg_end_t *end;
    void     **argtable;

    build_tf_argtable(&help, &help_json, &end, &argtable);
    fprintf(out, "Usage: false ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nDo nothing, unsuccessfully (exit code 1).\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-28s %s\n");
    arg_freetable(argtable, 3);
}

cmd_spec_t cmd_false_spec = {
    .name      = "false",
    .summary   = "do nothing, unsuccessfully",
    .long_help = "Exit with a status code of 1 (failure). "
                 "Useful as a loop condition that never terminates on its own.",
    .run         = false_run,
    .print_usage = false_print_usage,
};

void register_false_command(void) {
    register_command(&cmd_false_spec);
}
