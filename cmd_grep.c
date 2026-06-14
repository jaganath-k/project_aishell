#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <regex.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

typedef struct {
    regex_t regex;
    int     invert;
    int     line_num;
    int     count_only;
    int     files_only;
    int     recursive;
    int     show_path;   /* set when multiple files or -r */
} grep_opts_t;

/* Returns number of matching lines; prints them (unless count_only/files_only). */
static long search_stream_grep(FILE *fp, const char *path,
                                const grep_opts_t *opts)
{
    char   *line = NULL;
    size_t  cap  = 0;
    ssize_t n;
    long    lineno  = 0;
    long    matched = 0;

    while ((n = getline(&line, &cap, fp)) != -1) {
        lineno++;
        if (n > 0 && line[n - 1] == '\n') line[--n] = '\0';

        regmatch_t m;
        int hit = (regexec(&opts->regex, line, 1, &m, 0) == 0);
        if (opts->invert) hit = !hit;
        if (!hit) continue;

        matched++;
        if (opts->count_only || opts->files_only) continue;

        if (opts->show_path) printf("%s:", path);
        if (opts->line_num)  printf("%ld:", lineno);
        printf("%s\n", line);
    }
    free(line);
    return matched;
}

/* Forward declaration for mutual recursion */
static long grep_path(const char *path, grep_opts_t *opts);

static void grep_dir(const char *path, grep_opts_t *opts, long *total)
{
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "grep: %s: %s\n", path, strerror(errno));
        return;
    }
    struct dirent *ent;
    char child[PATH_MAX];
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        long n = grep_path(child, opts);
        if (n > 0) *total += n;
    }
    closedir(d);
}

static long grep_path(const char *path, grep_opts_t *opts)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "grep: %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        if (!opts->recursive) {
            fprintf(stderr, "grep: %s: Is a directory\n", path);
            return 0;
        }
        long total = 0;
        grep_dir(path, opts, &total);
        return total;
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "grep: %s: %s\n", path, strerror(errno));
        return -1;
    }
    long n = search_stream_grep(fp, path, opts);
    fclose(fp);

    if (opts->count_only) {
        if (opts->show_path) printf("%s:", path);
        printf("%ld\n", n);
    } else if (opts->files_only && n > 0) {
        printf("%s\n", path);
    }
    return n;
}

static void build_grep_argtable(arg_lit_t  **help,
                                 arg_lit_t  **help_json,
                                 arg_lit_t  **invert,
                                 arg_lit_t  **line_num,
                                 arg_lit_t  **count_only,
                                 arg_lit_t  **ignore_case,
                                 arg_lit_t  **files_only,
                                 arg_lit_t  **recursive,
                                 arg_str_t  **color,
                                 arg_str_t  **pattern,
                                 arg_file_t **files,
                                 arg_end_t  **end,
                                 void       ***argtable_out)
{
    *help       = arg_lit0("h", "help",               "show help and exit");
    *help_json  = arg_lit0(NULL, "help-json",         "print machine-readable help as JSON");
    *invert     = arg_lit0("v", "invert-match",       "print lines that do NOT match");
    *line_num   = arg_lit0("n", "line-number",        "prefix each output line with its line number");
    *count_only = arg_lit0("c", "count",              "print only a count of matching lines");
    *ignore_case= arg_lit0("i", "ignore-case",        "case-insensitive matching");
    *files_only = arg_lit0("l", "files-with-matches", "print only names of matching files");
    *recursive  = arg_lit0("r", "recursive",          "recurse into directories");
    *color      = arg_str0(NULL, "color",  "WHEN",    "colorize output (ignored)");
    *pattern    = arg_str1(NULL, NULL, "PATTERN",     "search pattern (POSIX ERE)");
    *files      = arg_filen(NULL, NULL, "FILE", 0, 64,"files to search (default: stdin)");
    *end        = arg_end(20);

    static void *argtable[13];
    argtable[0]  = *help;
    argtable[1]  = *help_json;
    argtable[2]  = *invert;
    argtable[3]  = *line_num;
    argtable[4]  = *count_only;
    argtable[5]  = *ignore_case;
    argtable[6]  = *files_only;
    argtable[7]  = *recursive;
    argtable[8]  = *color;
    argtable[9]  = *pattern;
    argtable[10] = *files;
    argtable[11] = *end;
    argtable[12] = NULL;
    *argtable_out = argtable;
}

void grep_print_usage(FILE *out);
extern cmd_spec_t cmd_grep_spec;

int grep_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json, *invert, *line_num, *count_only;
    arg_lit_t  *ignore_case, *files_only, *recursive;
    arg_str_t  *color, *pattern;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_grep_argtable(&help, &help_json, &invert, &line_num, &count_only,
                        &ignore_case, &files_only, &recursive,
                        &color, &pattern, &files, &end, &argtable);
    (void)color;
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 12);
        grep_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_grep_spec, argtable, stdout);
        arg_freetable(argtable, 12);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "grep");
        arg_freetable(argtable, 12);
        grep_print_usage(stdout);
        return 2;
    }

    int rflags = REG_EXTENDED | REG_NEWLINE;
    if (ignore_case->count > 0) rflags |= REG_ICASE;

    grep_opts_t opts = {
        .invert     = (invert->count > 0),
        .line_num   = (line_num->count > 0),
        .count_only = (count_only->count > 0),
        .files_only = (files_only->count > 0),
        .recursive  = (recursive->count > 0),
        .show_path  = (files->count > 1 || recursive->count > 0),
    };

    int rc = regcomp(&opts.regex, pattern->sval[0], rflags);
    if (rc != 0) {
        char errbuf[256];
        regerror(rc, &opts.regex, errbuf, sizeof(errbuf));
        fprintf(stderr, "grep: invalid regex '%s': %s\n",
                pattern->sval[0], errbuf);
        arg_freetable(argtable, 12);
        return 2;
    }

    arg_freetable(argtable, 11);

    int found_any = 0;

    if (files->count == 0) {
        if (opts.recursive) {
            opts.show_path = 1;
            long n = grep_path(".", &opts);
            if (n > 0) found_any = 1;
        } else {
            long n = search_stream_grep(stdin, NULL, &opts);
            if (opts.count_only) printf("%ld\n", n);
            if (n > 0) found_any = 1;
        }
    } else {
        for (int i = 0; i < files->count; i++) {
            long n = grep_path(files->filename[i], &opts);
            if (n > 0) found_any = 1;
        }
    }

    regfree(&opts.regex);
    return found_any ? 0 : 1;
}

void grep_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json, *invert, *line_num, *count_only;
    arg_lit_t  *ignore_case, *files_only, *recursive;
    arg_str_t  *color, *pattern;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_grep_argtable(&help, &help_json, &invert, &line_num, &count_only,
                        &ignore_case, &files_only, &recursive,
                        &color, &pattern, &files, &end, &argtable);
    fprintf(out, "Usage: grep ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nSearch for PATTERN in each FILE or stdin. PATTERN is a POSIX ERE.\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-28s %s\n");
    arg_freetable(argtable, 12);
}

cmd_spec_t cmd_grep_spec = {
    .name      = "grep",
    .summary   = "search for a pattern in files",
    .long_help = "Search for PATTERN in each FILE (or stdin). PATTERN is a POSIX "
                 "extended regular expression. Exit code: 0 if match found, "
                 "1 if no match, 2 on error. Composable in && / || chains.",
    .run         = grep_run,
    .print_usage = grep_print_usage,
};

void register_grep_command(void) {
    register_command(&cmd_grep_spec);
}
