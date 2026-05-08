#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <regex.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"
#include "edit_utils.h"

/* Build a new string with the first (or all, if global) regex matches replaced.
   Returns a heap-allocated result; caller must free(). */
static char *replace_in_line(regex_t *re, const char *line,
                              const char *repl, int global) {
    size_t rlen    = strlen(repl);
    size_t bufsz   = strlen(line) * 2 + 256;
    char  *buf     = malloc(bufsz);
    if (!buf) return NULL;
    size_t bpos    = 0;

    const char *p = line;
    regmatch_t  m;
    int replaced  = 0;

    while (*p) {
        if ((!global && replaced) || regexec(re, p, 1, &m, 0) != 0) {
            /* No more replacements: copy the rest of the line verbatim */
            size_t rest = strlen(p);
            while (bpos + rest >= bufsz) { bufsz *= 2; buf = realloc(buf, bufsz); }
            memcpy(buf + bpos, p, rest);
            bpos += rest;
            break;
        }

        /* Copy text before the match */
        size_t pre = (size_t)m.rm_so;
        while (bpos + pre + rlen >= bufsz) { bufsz *= 2; buf = realloc(buf, bufsz); }
        memcpy(buf + bpos, p, pre);
        bpos += pre;

        /* Copy replacement */
        memcpy(buf + bpos, repl, rlen);
        bpos += rlen;

        if (m.rm_eo == m.rm_so) {
            /* Zero-length match: advance one char to avoid an infinite loop */
            while (bpos + 1 >= bufsz) { bufsz *= 2; buf = realloc(buf, bufsz); }
            if (*p) buf[bpos++] = *p++;
        } else {
            p += m.rm_eo;
        }
        replaced++;
    }

    buf[bpos] = '\0';
    return buf;
}

static void build_edit_replace_argtable(
        arg_lit_t  **help,
        arg_lit_t  **help_json,
        arg_str_t  **pattern,
        arg_str_t  **replacement,
        arg_lit_t  **global,
        arg_lit_t  **ignore_case,
        arg_file_t **file,
        arg_end_t  **end,
        void       ***argtable_out)
{
    *help        = arg_lit0("h", "help",        "show help and exit");
    *help_json   = arg_lit0(NULL, "help-json",  "print machine-readable help as JSON");
    *pattern     = arg_str1("p", "pattern",     "PATTERN", "POSIX extended regex to search for");
    *replacement = arg_str1("r", "replacement", "TEXT",    "replacement text");
    *global      = arg_lit0("g", "global",      "replace all occurrences per line (default: first)");
    *ignore_case = arg_lit0("i", "ignore-case", "case-insensitive matching");
    *file        = arg_filen(NULL, NULL, "FILE", 1, 1, "file to edit");
    *end         = arg_end(20);

    static void *argtable[9];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *pattern;
    argtable[3] = *replacement;
    argtable[4] = *global;
    argtable[5] = *ignore_case;
    argtable[6] = *file;
    argtable[7] = *end;
    argtable[8] = NULL;
    *argtable_out = argtable;
}

void edit_replace_print_usage(FILE *out);
extern cmd_spec_t cmd_edit_replace_spec;

int edit_replace_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json, *global, *ignore_case;
    arg_str_t  *pattern, *replacement;
    arg_file_t *file;
    arg_end_t  *end;
    void      **argtable;

    build_edit_replace_argtable(&help, &help_json, &pattern, &replacement,
                                 &global, &ignore_case, &file, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 8);
        edit_replace_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_edit_replace_spec, argtable, stdout);
        arg_freetable(argtable, 8);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "edit-replace");
        arg_freetable(argtable, 8);
        edit_replace_print_usage(stdout);
        return 1;
    }

    const char *pat   = pattern->sval[0];
    const char *repl  = replacement->sval[0];
    int  glob_flag    = (global->count > 0);
    int  icase_flag   = (ignore_case->count > 0);
    const char *path  = file->filename[0];

    int cflags = REG_EXTENDED | (icase_flag ? REG_ICASE : 0);
    regex_t re;
    int rc = regcomp(&re, pat, cflags);
    if (rc != 0) {
        char errbuf[256];
        regerror(rc, &re, errbuf, sizeof(errbuf));
        fprintf(stderr, "edit-replace: bad pattern: %s\n", errbuf);
        arg_freetable(argtable, 8);
        return 1;
    }

    int     count;
    char  **lines = edit_read_lines(path, &count);
    if (!lines) {
        fprintf(stderr, "edit-replace: %s: %s\n", path, strerror(errno));
        regfree(&re);
        arg_freetable(argtable, 8);
        return 1;
    }

    int changed = 0;
    for (int i = 0; i < count; i++) {
        char *newline = replace_in_line(&re, lines[i], repl, glob_flag);
        if (!newline) continue;
        if (strcmp(newline, lines[i]) != 0) {
            free(lines[i]);
            lines[i] = newline;
            changed++;
        } else {
            free(newline);
        }
    }

    int ret = 0;
    if (changed > 0) {
        if (edit_write_lines(path, lines, count) < 0) {
            fprintf(stderr, "edit-replace: %s: %s\n", path, strerror(errno));
            ret = 1;
        } else {
            printf("%d line(s) modified\n", changed);
        }
    } else {
        printf("no matches found\n");
    }

    regfree(&re);
    edit_free_lines(lines, count);
    arg_freetable(argtable, 8);
    return ret;
}

void edit_replace_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json, *global, *ignore_case;
    arg_str_t  *pattern, *replacement;
    arg_file_t *file;
    arg_end_t  *end;
    void      **argtable;

    build_edit_replace_argtable(&help, &help_json, &pattern, &replacement,
                                 &global, &ignore_case, &file, &end, &argtable);
    fprintf(out, "Usage: edit-replace ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nFind and replace text in FILE using a POSIX extended regex.\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-28s %s\n");
    arg_freetable(argtable, 8);
}

cmd_spec_t cmd_edit_replace_spec = {
    .name      = "edit-replace",
    .summary   = "find and replace text in a file using a regex pattern",
    .long_help = "Search every line of FILE for PATTERN (POSIX extended regex) and "
                 "replace with TEXT. By default replaces only the first match per line; "
                 "use -g to replace all. Use -i for case-insensitive matching. "
                 "The file is updated atomically via a temp file and rename.",
    .run         = edit_replace_run,
    .print_usage = edit_replace_print_usage,
};

void register_edit_replace_command(void) {
    register_command(&cmd_edit_replace_spec);
}
