#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

static void build_uniq_argtable(arg_lit_t  **help,
                                 arg_lit_t  **help_json,
                                 arg_lit_t  **count,
                                 arg_lit_t  **repeated,
                                 arg_lit_t  **unique,
                                 arg_lit_t  **ignore_case,
                                 arg_file_t **file,
                                 arg_end_t  **end,
                                 void       ***argtable_out)
{
    *help        = arg_lit0("h", "help",        "show help and exit");
    *help_json   = arg_lit0(NULL, "help-json",  "print machine-readable help as JSON");
    *count       = arg_lit0("c", "count",       "prefix lines by the number of occurrences");
    *repeated    = arg_lit0("d", "repeated",    "only print duplicate lines, one for each group");
    *unique      = arg_lit0("u", "unique",      "only print unique lines");
    *ignore_case = arg_lit0("i", "ignore-case", "ignore differences in case when comparing");
    *file        = arg_file0(NULL, NULL, "FILE", "input file (default: stdin)");
    *end         = arg_end(20);

    static void *argtable[9];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *count;
    argtable[3] = *repeated;
    argtable[4] = *unique;
    argtable[5] = *ignore_case;
    argtable[6] = *file;
    argtable[7] = *end;
    argtable[8] = NULL;
    *argtable_out = argtable;
}

void uniq_print_usage(FILE *out);
extern cmd_spec_t cmd_uniq_spec;

static int run_uniq(FILE *in, int do_count, int do_repeated, int do_unique, int icase) {
    char *prev      = NULL;
    int   run_count = 0;
    char  buf[8192];

    while (fgets(buf, sizeof(buf), in)) {
        if (!prev) {
            prev = strdup(buf);
            run_count = 1;
            continue;
        }
        int same = icase ? (strcasecmp(prev, buf) == 0) : (strcmp(prev, buf) == 0);
        if (same) {
            run_count++;
        } else {
            int emit = (!do_repeated && !do_unique) ||
                       (do_repeated && run_count > 1) ||
                       (do_unique   && run_count == 1);
            if (emit) {
                if (do_count) printf("%7d %s", run_count, prev);
                else          fputs(prev, stdout);
            }
            free(prev);
            prev = strdup(buf);
            run_count = 1;
        }
    }
    if (prev) {
        int emit = (!do_repeated && !do_unique) ||
                   (do_repeated && run_count > 1) ||
                   (do_unique   && run_count == 1);
        if (emit) {
            if (do_count) printf("%7d %s", run_count, prev);
            else          fputs(prev, stdout);
        }
        free(prev);
    }
    return 0;
}

int uniq_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json, *count, *repeated, *unique, *ignore_case;
    arg_file_t *file;
    arg_end_t  *end;
    void      **argtable;

    build_uniq_argtable(&help, &help_json, &count, &repeated, &unique,
                        &ignore_case, &file, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 8);
        uniq_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_uniq_spec, argtable, stdout);
        arg_freetable(argtable, 8);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "uniq");
        arg_freetable(argtable, 8);
        uniq_print_usage(stdout);
        return 1;
    }

    int do_count    = (count->count > 0);
    int do_repeated = (repeated->count > 0);
    int do_unique   = (unique->count > 0);
    int icase       = (ignore_case->count > 0);

    int ret;
    if (file->count == 0) {
        ret = run_uniq(stdin, do_count, do_repeated, do_unique, icase);
    } else {
        FILE *f = fopen(file->filename[0], "r");
        if (!f) {
            fprintf(stderr, "uniq: %s: %s\n", file->filename[0], strerror(errno));
            arg_freetable(argtable, 8);
            return 1;
        }
        ret = run_uniq(f, do_count, do_repeated, do_unique, icase);
        fclose(f);
    }

    arg_freetable(argtable, 8);
    return ret;
}

void uniq_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json, *count, *repeated, *unique, *ignore_case;
    arg_file_t *file;
    arg_end_t  *end;
    void      **argtable;

    build_uniq_argtable(&help, &help_json, &count, &repeated, &unique,
                        &ignore_case, &file, &end, &argtable);
    fprintf(out, "Usage: uniq ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nFilter adjacent matching lines from FILE (or stdin) to stdout.\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-28s %s\n");
    arg_freetable(argtable, 8);
}

cmd_spec_t cmd_uniq_spec = {
    .name      = "uniq",
    .summary   = "report or filter out repeated adjacent lines",
    .long_help = "Filter adjacent matching lines from FILE (or stdin) and write to stdout. "
                 "Without options, output one copy of each group of adjacent equal lines. "
                 "Use -c to prefix each line with its occurrence count, -d to print only "
                 "duplicate lines, -u to print only unique lines, -i to ignore case.",
    .run         = uniq_run,
    .print_usage = uniq_print_usage,
};

void register_uniq_command(void) {
    register_command(&cmd_uniq_spec);
}
