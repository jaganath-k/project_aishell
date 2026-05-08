#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <regex.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

/* -------------------------------------------------------------------------
 * JSON string output helper
 * ---------------------------------------------------------------------- */

static void json_putn(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  fputs("\\\"", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        case '\n': fputs("\\n",  stdout); break;
        case '\r': fputs("\\r",  stdout); break;
        case '\t': fputs("\\t",  stdout); break;
        default:
            if (c < 0x20) printf("\\u%04x", c);
            else          putchar(c);
        }
    }
}

static void json_puts(const char *s) { json_putn(s, strlen(s)); }

/* -------------------------------------------------------------------------
 * Search state
 * ---------------------------------------------------------------------- */

typedef struct {
    const char *pattern;
    int   fixed_strings;
    int   ignore_case;
    int   json_out;
    int   files_only;
    int   recursive;
    int   show_path;
    regex_t regex;
    int   total_matched;
} rg_opts_t;

/* -------------------------------------------------------------------------
 * Output
 * ---------------------------------------------------------------------- */

static void emit_match(const char *path, long lineno,
                       const char *line, size_t mstart, size_t mend,
                       const rg_opts_t *opts)
{
    if (opts->json_out) {
        printf("{\"type\":\"match\",\"data\":{");
        printf("\"path\":\"");   json_puts(path);            printf("\",");
        printf("\"line_number\":%ld,", lineno);
        printf("\"line_text\":\""); json_puts(line);          printf("\",");
        printf("\"submatches\":[{\"text\":\"");
        json_putn(line + mstart, mend - mstart);
        printf("\",\"start\":%zu,\"end\":%zu}]}}\n", mstart, mend);
    } else {
        if (opts->show_path) printf("%s:", path);
        printf("%ld:%s\n", lineno, line);
    }
}

/* -------------------------------------------------------------------------
 * Line matching
 * ---------------------------------------------------------------------- */

static int match_line(const char *line, const rg_opts_t *opts,
                      size_t *out_start, size_t *out_end)
{
    if (opts->fixed_strings) {
        const char *p = opts->ignore_case
            ? strcasestr(line, opts->pattern)
            : strstr(line, opts->pattern);
        if (!p) return 0;
        *out_start = (size_t)(p - line);
        *out_end   = *out_start + strlen(opts->pattern);
        return 1;
    } else {
        regmatch_t m;
        if (regexec(&opts->regex, line, 1, &m, 0) != 0) return 0;
        *out_start = (size_t)m.rm_so;
        *out_end   = (size_t)m.rm_eo;
        return 1;
    }
}

/* -------------------------------------------------------------------------
 * Stream search  (returns 1=matched, 0=no match, -1=error)
 * ---------------------------------------------------------------------- */

static int search_stream(FILE *fp, const char *display_path, rg_opts_t *opts)
{
    char   *line = NULL;
    size_t  cap  = 0;
    ssize_t len;
    long    lineno    = 0;
    int     found_any = 0;

    while ((len = getline(&line, &cap, fp)) != -1) {
        lineno++;
        if (len > 0 && line[len - 1] == '\n')
            line[--len] = '\0';

        size_t ms = 0, me = 0;
        if (!match_line(line, opts, &ms, &me))
            continue;

        found_any = 1;
        opts->total_matched++;

        if (opts->files_only) {
            if (opts->json_out) {
                printf("{\"type\":\"match\",\"data\":{\"path\":\"");
                json_puts(display_path);
                printf("\"}}\n");
            } else {
                printf("%s\n", display_path);
            }
            break;
        }

        emit_match(display_path, lineno, line, ms, me, opts);
    }

    free(line);
    return found_any;
}

/* -------------------------------------------------------------------------
 * File / directory dispatch
 * ---------------------------------------------------------------------- */

static int search_file(const char *path, rg_opts_t *opts);

static int search_dir(const char *path, rg_opts_t *opts)
{
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "rg: cannot open directory '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    char child[PATH_MAX];
    struct dirent *ent;
    int found_any = 0;

    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.')
            continue;
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (search_file(child, opts) > 0)
            found_any = 1;
    }

    closedir(d);
    return found_any;
}

static int search_file(const char *path, rg_opts_t *opts)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "rg: cannot stat '%s': %s\n", path, strerror(errno));
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        if (!opts->recursive) {
            fprintf(stderr,
                    "rg: '%s' is a directory (use -r to search recursively)\n",
                    path);
            return 0;
        }
        return search_dir(path, opts);
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "rg: cannot open '%s': %s\n", path, strerror(errno));
        return -1;
    }
    int r = search_stream(fp, path, opts);
    fclose(fp);
    return r;
}

/* -------------------------------------------------------------------------
 * Argtable
 * ---------------------------------------------------------------------- */

static void build_rg_argtable(arg_lit_t  **help,
                               arg_lit_t  **help_json,
                               arg_lit_t  **fixed,
                               arg_lit_t  **json_out,
                               arg_lit_t  **icase,
                               arg_lit_t  **files_only,
                               arg_lit_t  **recursive,
                               arg_str_t  **pattern,
                               arg_file_t **paths,
                               arg_end_t  **end,
                               void       ***argtable_out)
{
    *help       = arg_lit0("h", "help",              "show help and exit");
    *help_json  = arg_lit0(NULL, "help-json",        "print machine-readable help as JSON");
    *fixed      = arg_lit0("F", "fixed-strings",     "treat PATTERN as literal string, not regex");
    *json_out   = arg_lit0(NULL, "json",             "output matches in JSON format");
    *icase      = arg_lit0("i", "ignore-case",       "case-insensitive matching");
    *files_only = arg_lit0("l", "files-with-matches","print only names of files with matches");
    *recursive  = arg_lit0("r", "recursive",         "recursively search directories");
    *pattern    = arg_str1(NULL, NULL, "PATTERN",    "search pattern (POSIX ERE or literal with -F)");
    *paths      = arg_filen(NULL, NULL, "PATH", 0, 64,
                             "files or directories to search (default: stdin)");
    *end        = arg_end(20);

    static void *argtable[11];
    argtable[0]  = *help;
    argtable[1]  = *help_json;
    argtable[2]  = *fixed;
    argtable[3]  = *json_out;
    argtable[4]  = *icase;
    argtable[5]  = *files_only;
    argtable[6]  = *recursive;
    argtable[7]  = *pattern;
    argtable[8]  = *paths;
    argtable[9]  = *end;
    argtable[10] = NULL;

    *argtable_out = argtable;
}

void rg_print_usage(FILE *out);
extern cmd_spec_t cmd_rg_spec;

/* -------------------------------------------------------------------------
 * rg_run
 * ---------------------------------------------------------------------- */

int rg_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json, *fixed, *json_out, *icase, *files_only, *recursive;
    arg_str_t  *pattern;
    arg_file_t *paths;
    arg_end_t  *end;
    void      **argtable;

    build_rg_argtable(&help, &help_json, &fixed, &json_out, &icase,
                      &files_only, &recursive, &pattern, &paths, &end, &argtable);

    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 10);
        rg_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_rg_spec, argtable, stdout);
        arg_freetable(argtable, 10);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "rg");
        arg_freetable(argtable, 10);
        rg_print_usage(stdout);
        return 2;
    }

    rg_opts_t opts = {
        .pattern       = pattern->sval[0],
        .fixed_strings = (fixed->count > 0),
        .ignore_case   = (icase->count  > 0),
        .json_out      = (json_out->count > 0),
        .files_only    = (files_only->count > 0),
        .recursive     = (recursive->count > 0),
        .show_path     = (paths->count > 0),
        .total_matched = 0,
    };

    if (!opts.fixed_strings) {
        int rflags = REG_EXTENDED | REG_NEWLINE;
        if (opts.ignore_case) rflags |= REG_ICASE;
        int rc = regcomp(&opts.regex, opts.pattern, rflags);
        if (rc != 0) {
            char errbuf[256];
            regerror(rc, &opts.regex, errbuf, sizeof(errbuf));
            fprintf(stderr, "rg: invalid regex '%s': %s\n",
                    opts.pattern, errbuf);
            arg_freetable(argtable, 10);
            return 2;
        }
    }

    int found_any = 0;

    if (paths->count == 0) {
        if (opts.recursive) {
            /* Default to current directory when -r and no paths given. */
            opts.show_path = 1;
            if (search_file(".", &opts) > 0) found_any = 1;
        } else {
            if (search_stream(stdin, "(stdin)", &opts) > 0) found_any = 1;
        }
    } else {
        for (int i = 0; i < paths->count; i++) {
            if (search_file(paths->filename[i], &opts) > 0)
                found_any = 1;
        }
    }

    if (!opts.fixed_strings)
        regfree(&opts.regex);

    arg_freetable(argtable, 10);

    /* Exit codes: 0 = match found, 1 = no match, 2 = error (grep convention). */
    return found_any ? 0 : 1;
}

/* -------------------------------------------------------------------------
 * rg_print_usage
 * ---------------------------------------------------------------------- */

void rg_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json, *fixed, *json_out, *icase, *files_only, *recursive;
    arg_str_t  *pattern;
    arg_file_t *paths;
    arg_end_t  *end;
    void      **argtable;

    build_rg_argtable(&help, &help_json, &fixed, &json_out, &icase,
                      &files_only, &recursive, &pattern, &paths, &end, &argtable);

    fprintf(out, "Usage: rg ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-28s %s\n");

    arg_freetable(argtable, 10);
}

cmd_spec_t cmd_rg_spec = {
    .name      = "rg",
    .summary   = "search files for a pattern",
    .long_help = "Search for PATTERN in each PATH (or stdin). PATTERN is a POSIX "
                 "extended regex by default; use -F for literal string matching. "
                 "Use --json for machine-readable output, -r to recurse directories.",
    .run         = rg_run,
    .print_usage = rg_print_usage,
};

void register_rg_command(void) {
    register_command(&cmd_rg_spec);
}
