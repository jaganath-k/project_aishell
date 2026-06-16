/* cmd_edit.c — Unified `edit` command: subcommand-based file editing.
 *
 * Usage:
 *   edit replace-line FILE N TEXT     — replace line N (1-indexed) with TEXT
 *   edit insert-line  FILE N TEXT     — insert TEXT before line N
 *   edit delete-line  FILE N          — delete line N
 *   edit replace      FILE PAT REPL   — global literal-string replace
 *
 * Max file size for in-memory editing: 10 MB.
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"
#include "edit_utils.h"
#include "cmd_edit.h"

#define EDIT_MAX_BYTES (10 * 1024 * 1024)   /* 10 MB */

/* ── Safety: reject paths containing ".." ─────────────────────────── */
static int edit_path_safe(const char *path, char *errbuf, size_t errsz) {
    if (strstr(path, "..")) {
        snprintf(errbuf, errsz, "path traversal rejected: '%s'", path);
        return 0;
    }
    return 1;
}

/* ── edit_op_replace_line ─────────────────────────────────────────── */
int edit_op_replace_line(const char *path, int linenum, const char *text,
                         char *errbuf, size_t errsz) {
    errbuf[0] = '\0';
    if (!edit_path_safe(path, errbuf, errsz)) return -1;

    int    count;
    char **lines = edit_read_lines(path, &count);
    if (!lines) {
        snprintf(errbuf, errsz, "%s: %s", path, strerror(errno));
        return -1;
    }
    if (linenum < 1 || linenum > count) {
        snprintf(errbuf, errsz, "line %d out of range (file has %d line%s)",
                 linenum, count, count == 1 ? "" : "s");
        edit_free_lines(lines, count);
        return -1;
    }
    char *newline = edit_ensure_newline(text);
    if (!newline) {
        edit_free_lines(lines, count);
        snprintf(errbuf, errsz, "out of memory");
        return -1;
    }
    free(lines[linenum - 1]);
    lines[linenum - 1] = newline;

    int rc = edit_write_lines(path, lines, count);
    if (rc < 0)
        snprintf(errbuf, errsz, "%s: write failed: %s", path, strerror(errno));
    edit_free_lines(lines, count);
    return rc;
}

/* ── edit_op_insert_line ──────────────────────────────────────────── */
int edit_op_insert_line(const char *path, int linenum, const char *text,
                        char *errbuf, size_t errsz) {
    errbuf[0] = '\0';
    if (!edit_path_safe(path, errbuf, errsz)) return -1;

    int    count;
    char **lines = edit_read_lines(path, &count);
    if (!lines && errno != 0) {
        /* File doesn't exist — allow insert at line 1 to create it */
        if (linenum != 1) {
            snprintf(errbuf, errsz, "%s: %s", path, strerror(errno));
            return -1;
        }
        count = 0;
        lines = malloc(sizeof(char *));
        if (!lines) { snprintf(errbuf, errsz, "out of memory"); return -1; }
    }

    /* linenum == count+1 means append at end; valid range: 1..count+1 */
    if (linenum < 1 || linenum > count + 1) {
        snprintf(errbuf, errsz, "line %d out of range (file has %d line%s)",
                 linenum, count, count == 1 ? "" : "s");
        edit_free_lines(lines, count);
        return -1;
    }

    char *newline = edit_ensure_newline(text);
    if (!newline) {
        edit_free_lines(lines, count);
        snprintf(errbuf, errsz, "out of memory");
        return -1;
    }

    char **nlines = malloc((size_t)(count + 1) * sizeof(char *));
    if (!nlines) {
        free(newline);
        edit_free_lines(lines, count);
        snprintf(errbuf, errsz, "out of memory");
        return -1;
    }

    int ins = linenum - 1;   /* 0-indexed insertion point */
    for (int i = 0; i < ins;       i++) nlines[i]     = lines[i];
    nlines[ins] = newline;
    for (int i = ins; i < count;   i++) nlines[i + 1] = lines[i];
    free(lines);   /* pointers moved into nlines; don't free contents */

    int rc = edit_write_lines(path, nlines, count + 1);
    if (rc < 0)
        snprintf(errbuf, errsz, "%s: write failed: %s", path, strerror(errno));
    edit_free_lines(nlines, count + 1);
    return rc;
}

/* ── edit_op_delete_line ──────────────────────────────────────────── */
int edit_op_delete_line(const char *path, int linenum,
                        char *errbuf, size_t errsz) {
    errbuf[0] = '\0';
    if (!edit_path_safe(path, errbuf, errsz)) return -1;

    int    count;
    char **lines = edit_read_lines(path, &count);
    if (!lines) {
        snprintf(errbuf, errsz, "%s: %s", path, strerror(errno));
        return -1;
    }
    if (linenum < 1 || linenum > count) {
        snprintf(errbuf, errsz, "line %d out of range (file has %d line%s)",
                 linenum, count, count == 1 ? "" : "s");
        edit_free_lines(lines, count);
        return -1;
    }

    free(lines[linenum - 1]);
    for (int i = linenum - 1; i < count - 1; i++)
        lines[i] = lines[i + 1];

    int rc = edit_write_lines(path, lines, count - 1);
    if (rc < 0)
        snprintf(errbuf, errsz, "%s: write failed: %s", path, strerror(errno));
    edit_free_lines(lines, count - 1);
    return rc;
}

/* ── edit_op_replace ──────────────────────────────────────────────── */
int edit_op_replace(const char *path, const char *pattern,
                    const char *replacement, int *nreplaced,
                    char *errbuf, size_t errsz) {
    errbuf[0] = '\0';
    *nreplaced = 0;
    if (!edit_path_safe(path, errbuf, errsz)) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(errbuf, errsz, "%s: %s", path, strerror(errno));
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);

    if (fsize > EDIT_MAX_BYTES) {
        fclose(f);
        snprintf(errbuf, errsz, "file too large (%ld bytes); use a real editor", fsize);
        return -1;
    }

    char *src = malloc((size_t)fsize + 1);
    if (!src) { fclose(f); snprintf(errbuf, errsz, "out of memory"); return -1; }
    size_t nr = fread(src, 1, (size_t)fsize, f);
    fclose(f);
    src[nr] = '\0';

    size_t patlen  = strlen(pattern);
    size_t replen  = strlen(replacement);
    if (patlen == 0) {
        free(src);
        snprintf(errbuf, errsz, "pattern must not be empty");
        return -1;
    }

    /* Count occurrences first to size the output buffer */
    int    n = 0;
    const char *p = src;
    while ((p = strstr(p, pattern)) != NULL) { n++; p += patlen; }

    if (n == 0) {
        free(src);
        *nreplaced = 0;
        return 0;   /* success: nothing to do */
    }

    /* Build the output buffer */
    size_t outsz = nr + (size_t)n * (replen > patlen ? replen - patlen : 0) + 1;
    char  *dst   = malloc(outsz);
    if (!dst) { free(src); snprintf(errbuf, errsz, "out of memory"); return -1; }

    const char *s   = src;
    char       *d   = dst;
    const char *hit;
    while ((hit = strstr(s, pattern)) != NULL) {
        size_t prefix = (size_t)(hit - s);
        memcpy(d, s, prefix);
        d += prefix;
        memcpy(d, replacement, replen);
        d += replen;
        s  = hit + patlen;
    }
    /* Copy tail */
    size_t tail = nr - (size_t)(s - src);
    memcpy(d, s, tail);
    d += tail;
    *d = '\0';

    free(src);

    /* Write via temp file + rename */
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *out = fopen(tmp, "wb");
    if (!out) {
        free(dst);
        snprintf(errbuf, errsz, "%s: %s", tmp, strerror(errno));
        return -1;
    }
    fwrite(dst, 1, (size_t)(d - dst), out);
    fclose(out);
    free(dst);

    if (rename(tmp, path) < 0) {
        remove(tmp);
        snprintf(errbuf, errsz, "rename failed: %s", strerror(errno));
        return -1;
    }
    *nreplaced = n;
    return 0;
}

/* ══════════════════ Shell command dispatcher ═════════════════════════ */

static int edit_usage_check_and_run(int argc, char **argv) {
    /* Minimal: argv[0]="edit", argv[1]=subcommand */
    if (argc < 2) {
        fprintf(stderr,
            "Usage: edit <subcommand> ...\n"
            "Subcommands:\n"
            "  replace-line FILE N TEXT\n"
            "  insert-line  FILE N TEXT\n"
            "  delete-line  FILE N\n"
            "  replace      FILE PATTERN REPLACEMENT\n");
        return 1;
    }

    const char *sub = argv[1];
    char errbuf[512] = "";

    if (strcmp(sub, "replace-line") == 0) {
        if (argc < 5) {
            fprintf(stderr, "Usage: edit replace-line FILE N TEXT\n");
            return 1;
        }
        int n = atoi(argv[3]);
        int rc = edit_op_replace_line(argv[2], n, argv[4], errbuf, sizeof(errbuf));
        if (rc < 0) { fprintf(stderr, "edit: %s\n", errbuf); return 1; }
        printf("edit: replaced line %d in %s\n", n, argv[2]);
        return 0;
    }

    if (strcmp(sub, "insert-line") == 0) {
        if (argc < 5) {
            fprintf(stderr, "Usage: edit insert-line FILE N TEXT\n");
            return 1;
        }
        int n = atoi(argv[3]);
        int rc = edit_op_insert_line(argv[2], n, argv[4], errbuf, sizeof(errbuf));
        if (rc < 0) { fprintf(stderr, "edit: %s\n", errbuf); return 1; }
        printf("edit: inserted line before %d in %s\n", n, argv[2]);
        return 0;
    }

    if (strcmp(sub, "delete-line") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: edit delete-line FILE N\n");
            return 1;
        }
        int n = atoi(argv[3]);
        int rc = edit_op_delete_line(argv[2], n, errbuf, sizeof(errbuf));
        if (rc < 0) { fprintf(stderr, "edit: %s\n", errbuf); return 1; }
        printf("edit: deleted line %d from %s\n", n, argv[2]);
        return 0;
    }

    if (strcmp(sub, "replace") == 0) {
        if (argc < 5) {
            fprintf(stderr, "Usage: edit replace FILE PATTERN REPLACEMENT\n");
            return 1;
        }
        int nrep = 0;
        int rc = edit_op_replace(argv[2], argv[3], argv[4], &nrep, errbuf, sizeof(errbuf));
        if (rc < 0) { fprintf(stderr, "edit: %s\n", errbuf); return 1; }
        printf("edit: replaced %d occurrence%s of '%s' with '%s' in %s\n",
               nrep, nrep == 1 ? "" : "s", argv[3], argv[4], argv[2]);
        return 0;
    }

    fprintf(stderr, "edit: unknown subcommand '%s'\n", sub);
    return 1;
}

/* ── argtable3 wrapper (supports --help and --help-json) ────────────── */

void edit_print_usage(FILE *out);
extern cmd_spec_t cmd_edit_spec;

static void build_edit_argtable(arg_lit_t **help,
                                 arg_lit_t **help_json,
                                 arg_end_t **end,
                                 void      ***argtable_out) {
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json","print machine-readable help as JSON");
    *end       = arg_end(20);

    static void *argtable[4];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *end;
    argtable[3] = NULL;
    *argtable_out = argtable;
}

static int edit_run(int argc, char **argv) {
    arg_lit_t *help, *help_json;
    arg_end_t *end;
    void     **argtable;
    build_edit_argtable(&help, &help_json, &end, &argtable);

    /* Only parse --help / --help-json if they appear before a subcommand.
     * Stop at first non-option token so that subcommand arguments are not
     * consumed by argtable's error handling. */
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 3);
        edit_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_edit_spec, argtable, stdout);
        arg_freetable(argtable, 3);
        return 0;
    }
    (void)nerrors;   /* subcommand errors handled by edit_usage_check_and_run */
    arg_freetable(argtable, 3);

    return edit_usage_check_and_run(argc, argv);
}

void edit_print_usage(FILE *out) {
    arg_lit_t *help, *help_json;
    arg_end_t *end;
    void     **argtable;
    build_edit_argtable(&help, &help_json, &end, &argtable);
    fprintf(out, "Usage: edit [--help] [--help-json] <subcommand> ...\n\n");
    fprintf(out, "Subcommands:\n");
    fprintf(out, "  replace-line FILE N TEXT        replace line N with TEXT\n");
    fprintf(out, "  insert-line  FILE N TEXT        insert TEXT before line N\n");
    fprintf(out, "  delete-line  FILE N             delete line N\n");
    fprintf(out, "  replace      FILE PAT REPL      global literal-string replace\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-28s %s\n");
    arg_freetable(argtable, 3);
}

cmd_spec_t cmd_edit_spec = {
    .name      = "edit",
    .summary   = "agent-friendly in-place file editing via subcommands",
    .long_help = "Subcommands: replace-line FILE N TEXT, insert-line FILE N TEXT, "
                 "delete-line FILE N, replace FILE PATTERN REPLACEMENT. "
                 "Line numbers are 1-indexed. Files are updated atomically "
                 "via a temp-file + rename. Max file size: 10 MB.",
    .run         = edit_run,
    .print_usage = edit_print_usage,
};

void register_edit_command(void) {
    register_command(&cmd_edit_spec);
}
