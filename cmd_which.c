#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

/* ── PATH search ──────────────────────────────────────────────────────── */

/* Search $PATH for NAME. If find_all, print every match; otherwise stop at
 * the first. Returns 1 if at least one match was found, 0 otherwise. */
static int search_path(const char *name, int find_all) {
    const char *path_env = getenv("PATH");
    if (!path_env) path_env = "/usr/local/bin:/usr/bin:/bin";

    char *path_copy = strdup(path_env);
    if (!path_copy) return 0;

    int found = 0;
    char *dir = strtok(path_copy, ":");
    while (dir) {
        char candidate[PATH_MAX];
        snprintf(candidate, sizeof(candidate), "%s/%s", dir, name);
        if (access(candidate, X_OK) == 0) {
            printf("%s\n", candidate);
            found = 1;
            if (!find_all) break;
        }
        dir = strtok(NULL, ":");
    }
    free(path_copy);
    return found;
}

/* ── Argtable ─────────────────────────────────────────────────────────── */

static void build_which_argtable(arg_lit_t  **help,
                                  arg_lit_t  **help_json,
                                  arg_lit_t  **all,
                                  arg_str_t  **names,
                                  arg_end_t  **end,
                                  void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json", "print machine-readable help as JSON");
    *all       = arg_lit0("a", "all",        "print all matching paths, not just the first");
    *names     = arg_strn(NULL, NULL, "NAME", 1, 64, "command name(s) to look up");
    *end       = arg_end(20);

    static void *argtable[6];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *all;
    argtable[3] = *names;
    argtable[4] = *end;
    argtable[5] = NULL;
    *argtable_out = argtable;
}

void which_print_usage(FILE *out);
extern cmd_spec_t cmd_which_spec;

/* ── which_run ────────────────────────────────────────────────────────── */

int which_run(int argc, char **argv) {
    arg_lit_t *help, *help_json, *all;
    arg_str_t *names;
    arg_end_t *end;
    void     **argtable;

    build_which_argtable(&help, &help_json, &all, &names, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 5);
        which_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_which_spec, argtable, stdout);
        arg_freetable(argtable, 5);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "which");
        arg_freetable(argtable, 5);
        which_print_usage(stdout);
        return 1;
    }

    int find_all = (all->count > 0);
    int nnames   = names->count;
    const char *name_arr[64];
    for (int i = 0; i < nnames && i < 64; i++)
        name_arr[i] = names->sval[i];

    arg_freetable(argtable, 5);

    int ret = 0;
    for (int i = 0; i < nnames; i++) {
        if (!search_path(name_arr[i], find_all))
            ret = 1;
    }
    return ret;
}

/* ── which_print_usage ────────────────────────────────────────────────── */

void which_print_usage(FILE *out) {
    arg_lit_t *help, *help_json, *all;
    arg_str_t *names;
    arg_end_t *end;
    void     **argtable;

    build_which_argtable(&help, &help_json, &all, &names, &end, &argtable);
    fprintf(out, "Usage: which ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nLocate a command in the filesystem PATH.\n");
    fprintf(out, "\n  Searches each directory in $PATH for NAME.\n");
    fprintf(out, "  Prints the first match (or all matches with -a).\n");
    fprintf(out, "  Exit code is 0 if all names were found, 1 otherwise.\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-28s %s\n");
    arg_freetable(argtable, 5);
}

cmd_spec_t cmd_which_spec = {
    .name      = "which",
    .summary   = "locate a command in PATH",
    .long_help = "For each NAME, search the directories in $PATH for an executable file. "
                 "Print the full path of the first match found. "
                 "With -a/--all, print all matches rather than just the first. "
                 "Exit code is 0 if every NAME was found, 1 if any was not.",
    .run         = which_run,
    .print_usage = which_print_usage,
};

void register_which_command(void) {
    register_command(&cmd_which_spec);
}
