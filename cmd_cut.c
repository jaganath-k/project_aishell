#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

#define MAX_FIELDS 1024

static void build_cut_argtable(arg_lit_t  **help,
                                arg_lit_t  **help_json,
                                arg_str_t  **delim,
                                arg_str_t  **fields,
                                arg_str_t  **chars,
                                arg_file_t **files,
                                arg_end_t  **end,
                                void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",        "show help and exit");
    *help_json = arg_lit0(NULL, "help-json",  "print machine-readable help as JSON");
    *delim     = arg_str0("d", "delimiter",   "DELIM", "field delimiter (default: TAB)");
    *fields    = arg_str0("f", "fields",      "LIST",  "select fields (e.g. 1, 1-3, 1,3-5)");
    *chars     = arg_str0("c", "characters",  "LIST",  "select character positions");
    *files     = arg_filen(NULL, NULL, "FILE...", 0, 64, "files to cut (default: stdin)");
    *end       = arg_end(20);

    static void *argtable[8];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *delim;
    argtable[3] = *fields;
    argtable[4] = *chars;
    argtable[5] = *files;
    argtable[6] = *end;
    argtable[7] = NULL;
    *argtable_out = argtable;
}

/* Parse "1", "1-3", "1-3,5", "-3", "3-" into selected[1..MAX_FIELDS]. */
static int parse_list(const char *spec, char selected[MAX_FIELDS + 1]) {
    char buf[4096];
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *tok = strtok(buf, ",");
    while (tok) {
        if (tok[0] == '-') {
            /* -M: 1 through M */
            int m = atoi(tok + 1);
            if (m < 1 || m > MAX_FIELDS) return -1;
            for (int i = 1; i <= m; i++) selected[i] = 1;
        } else {
            char *dash = strchr(tok + 1, '-');
            if (!dash) {
                /* N */
                int n = atoi(tok);
                if (n < 1 || n > MAX_FIELDS) return -1;
                selected[n] = 1;
            } else if (*(dash + 1) == '\0') {
                /* N- */
                *dash = '\0';
                int n = atoi(tok);
                if (n < 1 || n > MAX_FIELDS) return -1;
                for (int i = n; i <= MAX_FIELDS; i++) selected[i] = 1;
            } else {
                /* N-M */
                *dash = '\0';
                int n = atoi(tok);
                int m = atoi(dash + 1);
                if (n < 1 || m < 1 || n > MAX_FIELDS || m > MAX_FIELDS || n > m) return -1;
                for (int i = n; i <= m; i++) selected[i] = 1;
            }
        }
        tok = strtok(NULL, ",");
    }
    return 0;
}

static int run_cut_fields(FILE *in, char delim, const char selected[MAX_FIELDS + 1]) {
    char buf[65536];
    while (fgets(buf, sizeof(buf), in)) {
        size_t len = strlen(buf);
        int has_nl = (len > 0 && buf[len - 1] == '\n');
        if (has_nl) buf[--len] = '\0';

        int field = 1;
        int first_out = 1;
        const char *p = buf;
        for (;;) {
            const char *next = strchr(p, delim);
            size_t flen = next ? (size_t)(next - p) : strlen(p);
            if (field <= MAX_FIELDS && selected[field]) {
                if (!first_out) putchar(delim);
                fwrite(p, 1, flen, stdout);
                first_out = 0;
            }
            if (!next) break;
            p = next + 1;
            field++;
        }
        if (!first_out || has_nl) putchar('\n');
    }
    return 0;
}

static int run_cut_chars(FILE *in, const char selected[MAX_FIELDS + 1]) {
    char buf[65536];
    while (fgets(buf, sizeof(buf), in)) {
        size_t len = strlen(buf);
        int has_nl = (len > 0 && buf[len - 1] == '\n');
        size_t content_len = has_nl ? len - 1 : len;
        for (size_t i = 1; i <= content_len && i <= (size_t)MAX_FIELDS; i++) {
            if (selected[i]) putchar(buf[i - 1]);
        }
        if (has_nl) putchar('\n');
    }
    return 0;
}

void cut_print_usage(FILE *out);
extern cmd_spec_t cmd_cut_spec;

int cut_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json;
    arg_str_t  *delim, *fields, *chars;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_cut_argtable(&help, &help_json, &delim, &fields, &chars,
                       &files, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 7);
        cut_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_cut_spec, argtable, stdout);
        arg_freetable(argtable, 7);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "cut");
        arg_freetable(argtable, 7);
        cut_print_usage(stdout);
        return 1;
    }

    if (fields->count == 0 && chars->count == 0) {
        fprintf(stderr, "cut: you must specify a list of fields (-f) or characters (-c)\n");
        arg_freetable(argtable, 7);
        cut_print_usage(stdout);
        return 1;
    }
    if (fields->count > 0 && chars->count > 0) {
        fprintf(stderr, "cut: only one of -f or -c may be specified\n");
        arg_freetable(argtable, 7);
        cut_print_usage(stdout);
        return 1;
    }

    char delim_char = (delim->count > 0 && delim->sval[0][0]) ? delim->sval[0][0] : '\t';

    char selected[MAX_FIELDS + 1];
    memset(selected, 0, sizeof(selected));
    const char *list_str = (fields->count > 0) ? fields->sval[0] : chars->sval[0];
    if (parse_list(list_str, selected) < 0) {
        fprintf(stderr, "cut: invalid list format: %s\n", list_str);
        arg_freetable(argtable, 7);
        return 1;
    }

    int do_fields = (fields->count > 0);
    int ret = 0;

    if (files->count == 0) {
        if (do_fields) run_cut_fields(stdin,  delim_char, selected);
        else           run_cut_chars(stdin,  selected);
    } else {
        for (int i = 0; i < files->count; i++) {
            FILE *f = fopen(files->filename[i], "r");
            if (!f) {
                fprintf(stderr, "cut: %s: %s\n", files->filename[i], strerror(errno));
                ret = 1;
                continue;
            }
            if (do_fields) run_cut_fields(f, delim_char, selected);
            else           run_cut_chars(f, selected);
            fclose(f);
        }
    }

    arg_freetable(argtable, 7);
    return ret;
}

void cut_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json;
    arg_str_t  *delim, *fields, *chars;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_cut_argtable(&help, &help_json, &delim, &fields, &chars,
                       &files, &end, &argtable);
    fprintf(out, "Usage: cut ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nRemove sections from each line of FILE(s) or stdin.\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-28s %s\n");
    arg_freetable(argtable, 7);
}

cmd_spec_t cmd_cut_spec = {
    .name      = "cut",
    .summary   = "remove sections from each line of files",
    .long_help = "Print selected parts of lines from each FILE (or stdin). "
                 "Use -f LIST to select fields delimited by -d DELIM (default TAB), "
                 "or -c LIST to select character positions. "
                 "LIST is comma-separated ranges: N, N-M, N-, -M.",
    .run         = cut_run,
    .print_usage = cut_print_usage,
};

void register_cut_command(void) {
    register_command(&cmd_cut_spec);
}
