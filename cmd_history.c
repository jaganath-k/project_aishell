#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

/* ── History functions defined in aishell_main.c ─────────────────────── */
extern void        hist_add(const char *line);
extern const char *hist_get(int i);
extern int         hist_total(void);
extern void        hist_clear_all(void);

static void build_history_argtable(arg_lit_t **help,
                                    arg_lit_t **help_json,
                                    arg_lit_t **clear,
                                    arg_int_t **nlines,
                                    arg_end_t **end,
                                    void      ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json", "print machine-readable help as JSON");
    *clear     = arg_lit0("c", "clear",     "clear the history list");
    *nlines    = arg_int0("n", NULL, "N",   "show only the last N entries");
    *end       = arg_end(20);

    static void *argtable[6];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *clear;
    argtable[3] = *nlines;
    argtable[4] = *end;
    argtable[5] = NULL;
    *argtable_out = argtable;
}

void history_print_usage(FILE *out);
extern cmd_spec_t cmd_history_spec;

int history_run(int argc, char **argv)
{
    arg_lit_t *help, *help_json, *clear;
    arg_int_t *nlines;
    arg_end_t *end;
    void     **argtable;

    build_history_argtable(&help, &help_json, &clear, &nlines, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 5);
        history_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_history_spec, argtable, stdout);
        arg_freetable(argtable, 5);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "history");
        arg_freetable(argtable, 5);
        history_print_usage(stdout);
        return 1;
    }

    int do_clear = (clear->count > 0);
    int n = (nlines->count > 0) ? nlines->ival[0] : -1;
    arg_freetable(argtable, 5);

    if (do_clear) {
        hist_clear_all();
        return 0;
    }

    int total = hist_total();
    int start = 0;
    if (n > 0 && n < total) start = total - n;

    for (int i = start; i < total; i++)
        printf("%5d  %s\n", i + 1, hist_get(i));

    return 0;
}

void history_print_usage(FILE *out)
{
    arg_lit_t *help, *help_json, *clear;
    arg_int_t *nlines;
    arg_end_t *end;
    void     **argtable;

    build_history_argtable(&help, &help_json, &clear, &nlines, &end, &argtable);
    fprintf(out, "Usage: history ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nDisplay or manipulate the command history list.\n");
    fprintf(out, "  history          show all history with line numbers\n");
    fprintf(out, "  history -n N     show last N entries\n");
    fprintf(out, "  history -c       clear the history list\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-28s %s\n");
    arg_freetable(argtable, 5);
}

cmd_spec_t cmd_history_spec = {
    .name      = "history",
    .summary   = "display or clear the command history list",
    .long_help = "Without options, print all history entries with line numbers. "
                 "-n N limits output to the last N entries. "
                 "-c clears the in-memory history (does not affect the history file "
                 "until the shell exits).",
    .run         = history_run,
    .print_usage = history_print_usage,
};

void register_history_command(void) {
    register_command(&cmd_history_spec);
}
