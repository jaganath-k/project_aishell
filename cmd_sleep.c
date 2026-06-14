#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

/* ── Duration parser ──────────────────────────────────────────────────── */

/* Parse "NUMBER[SUFFIX]" and return seconds as double, or -1.0 on error. */
static double parse_duration(const char *arg) {
    if (!arg || *arg == '\0') return -1.0;

    char *end;
    double val = strtod(arg, &end);
    if (end == arg || val < 0.0) return -1.0;

    double factor = 1.0;
    switch (*end) {
        case '\0': case 's': factor =       1.0; end += (*end != '\0'); break;
        case 'm':            factor =      60.0; end++; break;
        case 'h':            factor =    3600.0; end++; break;
        case 'd':            factor =   86400.0; end++; break;
        default: return -1.0;
    }
    if (*end != '\0') return -1.0; /* trailing garbage */
    return val * factor;
}

/* ── nanosleep with EINTR retry ───────────────────────────────────────── */

static int do_sleep(double seconds) {
    struct timespec ts;
    ts.tv_sec  = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1.0e9);
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }

    while (nanosleep(&ts, &ts) < 0) {
        if (errno != EINTR) {
            perror("sleep: nanosleep");
            return -1;
        }
        /* ts updated with remaining time — loop to finish the sleep */
    }
    return 0;
}

/* ── Argtable ─────────────────────────────────────────────────────────── */

static void build_sleep_argtable(arg_lit_t **help,
                                  arg_lit_t **help_json,
                                  arg_str_t **durations,
                                  arg_end_t **end,
                                  void      ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json", "print machine-readable help as JSON");
    *durations = arg_strn(NULL, NULL, "NUMBER[SUFFIX]", 1, 20,
                          "duration(s) to sleep; SUFFIX: s(default) m h d");
    *end       = arg_end(20);

    static void *argtable[5];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *durations;
    argtable[3] = *end;
    argtable[4] = NULL;
    *argtable_out = argtable;
}

void sleep_print_usage(FILE *out);
extern cmd_spec_t cmd_sleep_spec;

/* ── sleep_run ────────────────────────────────────────────────────────── */

int sleep_run(int argc, char **argv) {
    arg_lit_t *help, *help_json;
    arg_str_t *durations;
    arg_end_t *end;
    void     **argtable;

    build_sleep_argtable(&help, &help_json, &durations, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 4);
        sleep_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_sleep_spec, argtable, stdout);
        arg_freetable(argtable, 4);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "sleep");
        arg_freetable(argtable, 4);
        sleep_print_usage(stdout);
        return 1;
    }

    /* Sum all durations */
    double total = 0.0;
    for (int i = 0; i < durations->count; i++) {
        double d = parse_duration(durations->sval[i]);
        if (d < 0.0) {
            fprintf(stderr, "sleep: invalid time interval '%s'\n",
                    durations->sval[i]);
            arg_freetable(argtable, 4);
            return 1;
        }
        total += d;
    }

    arg_freetable(argtable, 4);
    return do_sleep(total) < 0 ? 1 : 0;
}

/* ── sleep_print_usage ────────────────────────────────────────────────── */

void sleep_print_usage(FILE *out) {
    arg_lit_t *help, *help_json;
    arg_str_t *durations;
    arg_end_t *end;
    void     **argtable;

    build_sleep_argtable(&help, &help_json, &durations, &end, &argtable);
    fprintf(out, "Usage: sleep ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nPause for the specified duration(s), which are summed.\n");
    fprintf(out, "\n  Suffixes: s = seconds (default), m = minutes,"
                 " h = hours, d = days\n");
    fprintf(out, "  Fractional values are supported: 0.5s, 1.5m\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-28s %s\n");
    arg_freetable(argtable, 4);
}

cmd_spec_t cmd_sleep_spec = {
    .name      = "sleep",
    .summary   = "delay for a specified amount of time",
    .long_help = "Pause execution for the sum of the given durations. "
                 "Each argument is NUMBER[SUFFIX] where SUFFIX is s (seconds, default), "
                 "m (minutes), h (hours), or d (days). "
                 "Fractional values like 0.5 or 1.5m are accepted. "
                 "Multiple durations are summed: sleep 1s 30s waits 31 seconds.",
    .run         = sleep_run,
    .print_usage = sleep_print_usage,
};

void register_sleep_command(void) {
    register_command(&cmd_sleep_spec);
}
