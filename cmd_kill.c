#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <signal.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

/* -------------------------------------------------------------------------
 * Signal table
 * ---------------------------------------------------------------------- */

static const struct { const char *name; int num; } sig_table[] = {
    {"HUP",    1}, {"INT",    2}, {"QUIT",   3}, {"ILL",    4},
    {"TRAP",   5}, {"ABRT",   6}, {"BUS",    7}, {"FPE",    8},
    {"KILL",   9}, {"USR1",  10}, {"SEGV",  11}, {"USR2",  12},
    {"PIPE",  13}, {"ALRM",  14}, {"TERM",  15}, {"STKFLT",16},
    {"CHLD",  17}, {"CONT",  18}, {"STOP",  19}, {"TSTP",  20},
    {"TTIN",  21}, {"TTOU",  22}, {"URG",   23}, {"XCPU",  24},
    {"XFSZ",  25}, {"VTALRM",26}, {"PROF",  27}, {"WINCH", 28},
    {"IO",    29}, {"PWR",   30}, {"SYS",   31},
    {NULL, 0}
};

static int parse_signal(const char *spec) {
    char *end;
    long n = strtol(spec, &end, 10);
    if (*end == '\0' && n > 0 && n <= 31) return (int)n;
    const char *name = spec;
    if (strncasecmp(name, "SIG", 3) == 0) name += 3;
    for (int i = 0; sig_table[i].name; i++)
        if (strcasecmp(sig_table[i].name, name) == 0)
            return sig_table[i].num;
    return -1;
}

/* -------------------------------------------------------------------------
 * Argtable
 * ---------------------------------------------------------------------- */

static void build_kill_argtable(arg_lit_t  **help,
                                 arg_lit_t  **help_json,
                                 arg_str_t  **sig,
                                 arg_lit_t  **list,
                                 arg_int_t  **pids,
                                 arg_end_t  **end,
                                 void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json","print machine-readable help as JSON");
    *sig       = arg_str0("s", "signal",    "SIGNAL", "signal name or number (default: TERM)");
    *list      = arg_lit0("l", "list",      "list signal names and numbers");
    *pids      = arg_intn(NULL, NULL,       "PID", 0, 64, "process IDs to signal");
    *end       = arg_end(20);

    static void *argtable[7];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *sig;
    argtable[3] = *list;
    argtable[4] = *pids;
    argtable[5] = *end;
    argtable[6] = NULL;

    *argtable_out = argtable;
}

void kill_print_usage(FILE *out);
extern cmd_spec_t cmd_kill_spec;

/* -------------------------------------------------------------------------
 * kill_run
 * ---------------------------------------------------------------------- */

int kill_run(int argc, char **argv) {
    arg_lit_t *help, *help_json, *list;
    arg_str_t *sig;
    arg_int_t *pids;
    arg_end_t *end;
    void     **argtable;

    build_kill_argtable(&help, &help_json, &sig, &list, &pids, &end, &argtable);

    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 6);
        kill_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_kill_spec, argtable, stdout);
        arg_freetable(argtable, 6);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "kill");
        arg_freetable(argtable, 6);
        kill_print_usage(stdout);
        return 1;
    }

    if (list->count > 0) {
        for (int i = 0; sig_table[i].name; i++) {
            printf("%2d) SIG%-10s", sig_table[i].num, sig_table[i].name);
            if ((i + 1) % 4 == 0) putchar('\n');
        }
        putchar('\n');
        arg_freetable(argtable, 6);
        return 0;
    }

    int signum = SIGTERM;
    if (sig->count > 0) {
        signum = parse_signal(sig->sval[0]);
        if (signum < 0) {
            fprintf(stderr, "kill: unknown signal '%s'\n", sig->sval[0]);
            arg_freetable(argtable, 6);
            return 1;
        }
    }

    if (pids->count == 0) {
        fprintf(stderr, "kill: no PID specified\n");
        arg_freetable(argtable, 6);
        kill_print_usage(stdout);
        return 1;
    }

    int ret = 0;
    for (int i = 0; i < pids->count; i++) {
        /* The kill() syscall from <signal.h> — not a recursive call. */
        if (kill((pid_t)pids->ival[i], signum) != 0) {
            fprintf(stderr, "kill: (%d): %s\n", pids->ival[i], strerror(errno));
            ret = 1;
        }
    }

    arg_freetable(argtable, 6);
    return ret;
}

/* -------------------------------------------------------------------------
 * kill_print_usage
 * ---------------------------------------------------------------------- */

void kill_print_usage(FILE *out) {
    arg_lit_t *help, *help_json, *list;
    arg_str_t *sig;
    arg_int_t *pids;
    arg_end_t *end;
    void     **argtable;

    build_kill_argtable(&help, &help_json, &sig, &list, &pids, &end, &argtable);

    fprintf(out, "Usage: kill ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-22s %s\n");

    arg_freetable(argtable, 6);
}

cmd_spec_t cmd_kill_spec = {
    .name      = "kill",
    .summary   = "send a signal to a process",
    .long_help = "Send SIGNAL to each PID. SIGNAL may be a name (TERM, KILL, HUP ...) "
                 "or a number. Default signal is TERM. Use -l to list all signals.",
    .run         = kill_run,
    .print_usage = kill_print_usage,
};

void register_kill_command(void) {
    register_command(&cmd_kill_spec);
}
