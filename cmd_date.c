#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

#define DEFAULT_FMT "%a %b %e %H:%M:%S %Z %Y"

static void build_date_argtable(arg_lit_t **help,
                                 arg_lit_t **help_json,
                                 arg_lit_t **utc,
                                 arg_str_t **format,
                                 arg_end_t **end,
                                 void      ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json","print machine-readable help as JSON");
    *utc       = arg_lit0("u", "utc",       "display time in UTC instead of local time");
    *format    = arg_str0("f", "format", "FMT",
                          "strftime format string (default: \"" DEFAULT_FMT "\")");
    *end       = arg_end(20);

    static void *argtable[6];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *utc;
    argtable[3] = *format;
    argtable[4] = *end;
    argtable[5] = NULL;
    *argtable_out = argtable;
}

void date_print_usage(FILE *out);
extern cmd_spec_t cmd_date_spec;

int date_run(int argc, char **argv) {
    arg_lit_t *help, *help_json, *utc;
    arg_str_t *format;
    arg_end_t *end;
    void     **argtable;

    build_date_argtable(&help, &help_json, &utc, &format, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 5);
        date_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_date_spec, argtable, stdout);
        arg_freetable(argtable, 5);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "date");
        arg_freetable(argtable, 5);
        date_print_usage(stdout);
        return 1;
    }

    const char *fmt     = (format->count > 0) ? format->sval[0] : DEFAULT_FMT;
    int         use_utc = (utc->count > 0);

    time_t     now = time(NULL);
    struct tm *tm  = use_utc ? gmtime(&now) : localtime(&now);

    char buf[512];
    strftime(buf, sizeof(buf), fmt, tm);
    puts(buf);

    arg_freetable(argtable, 5);
    return 0;
}

void date_print_usage(FILE *out) {
    arg_lit_t *help, *help_json, *utc;
    arg_str_t *format;
    arg_end_t *end;
    void     **argtable;

    build_date_argtable(&help, &help_json, &utc, &format, &end, &argtable);
    fprintf(out, "Usage: date ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nPrint the current date and time.\n");
    fprintf(out, "\nstrftime format tokens: %%Y year, %%m month, %%d day, "
                 "%%H hour, %%M min, %%S sec, %%Z timezone.\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-30s %s\n");
    arg_freetable(argtable, 5);
}

cmd_spec_t cmd_date_spec = {
    .name      = "date",
    .summary   = "print the current date and time",
    .long_help = "Print the current date and time. "
                 "Use -u for UTC output. "
                 "Use -f FMT to specify a custom strftime format string, "
                 "e.g. -f \"%%Y-%%m-%%d\" for ISO 8601 date.",
    .run         = date_run,
    .print_usage = date_print_usage,
};

void register_date_command(void) {
    register_command(&cmd_date_spec);
}
