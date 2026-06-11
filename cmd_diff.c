#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

#define MAX_LINES    4096
#define CONTEXT_LINES 3

/* ── Line storage ─────────────────────────────────────────────────────── */

typedef struct { char **v; int n; } Lines;

static int read_lines(const char *path, Lines *L) {
    int use_stdin = (strcmp(path, "-") == 0);
    FILE *f = use_stdin ? stdin : fopen(path, "r");
    if (!f) {
        fprintf(stderr, "diff: %s: %s\n", path, strerror(errno));
        return -1;
    }
    L->v = malloc(MAX_LINES * sizeof(char *));
    if (!L->v) { if (!use_stdin) fclose(f); return -1; }
    L->n = 0;
    char *line = NULL; size_t cap = 0; ssize_t len;
    while (L->n < MAX_LINES && (len = getline(&line, &cap, f)) != -1) {
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
        L->v[L->n++] = strdup(line);
    }
    free(line);
    if (!use_stdin) fclose(f);
    return 0;
}

static void free_lines(Lines *L) {
    for (int i = 0; i < L->n; i++) free(L->v[i]);
    free(L->v);
}

/* ── Line comparison ──────────────────────────────────────────────────── */

static int lines_eq(const char *a, const char *b, int icase, int ignsp) {
    if (ignsp) {
        const char *p = a, *q = b;
        while (1) {
            while (isspace((unsigned char)*p)) p++;
            while (isspace((unsigned char)*q)) q++;
            if (!*p && !*q) return 1;
            if (!*p || !*q) return 0;
            char cp = icase ? (char)tolower((unsigned char)*p) : *p;
            char cq = icase ? (char)tolower((unsigned char)*q) : *q;
            if (cp != cq) return 0;
            p++; q++;
        }
    }
    return icase ? (strcasecmp(a, b) == 0) : (strcmp(a, b) == 0);
}

/* ── LCS diff engine ──────────────────────────────────────────────────── */

typedef enum { OP_EQ, OP_DEL, OP_ADD } Op;
typedef struct { Op op; int l1; int l2; } Edit;

static Edit *lcs_diff(const Lines *A, const Lines *B,
                      int icase, int ignsp, int *nout)
{
    int n1 = A->n, n2 = B->n;
    /* DP table: (n1+1) × (n2+1) shorts — fits MAX_LINES=4096 on heap (~33 MB) */
    short *dp = calloc((size_t)(n1+1) * (n2+1), sizeof(short));
    if (!dp) return NULL;

    for (int i = 1; i <= n1; i++) {
        for (int j = 1; j <= n2; j++) {
            if (lines_eq(A->v[i-1], B->v[j-1], icase, ignsp)) {
                dp[i*(n2+1)+j] = (short)(dp[(i-1)*(n2+1)+(j-1)] + 1);
            } else {
                short u = dp[(i-1)*(n2+1)+j];
                short l = dp[i*(n2+1)+(j-1)];
                dp[i*(n2+1)+j] = u > l ? u : l;
            }
        }
    }

    /* Traceback */
    Edit *ops = malloc((size_t)(n1 + n2 + 1) * sizeof(Edit));
    if (!ops) { free(dp); return NULL; }
    int nops = 0;
    int i = n1, j = n2;
    while (i > 0 || j > 0) {
        if (i > 0 && j > 0 && lines_eq(A->v[i-1], B->v[j-1], icase, ignsp)) {
            ops[nops++] = (Edit){ OP_EQ, i-1, j-1 };
            i--; j--;
        } else if (j > 0 &&
                   (i == 0 || dp[(i-1)*(n2+1)+j] <= dp[i*(n2+1)+(j-1)])) {
            ops[nops++] = (Edit){ OP_ADD, -1, j-1 };
            j--;
        } else {
            ops[nops++] = (Edit){ OP_DEL, i-1, -1 };
            i--;
        }
    }
    free(dp);

    /* Reverse in-place */
    for (int k = 0; k < nops/2; k++) {
        Edit t = ops[k]; ops[k] = ops[nops-1-k]; ops[nops-1-k] = t;
    }
    *nout = nops;
    return ops;
}

/* ── Traditional diff output ──────────────────────────────────────────── */

static void emit_traditional(const Edit *ops, int nops,
                              const Lines *A, const Lines *B)
{
    int i = 0;
    while (i < nops) {
        if (ops[i].op == OP_EQ) { i++; continue; }

        int start = i;
        while (i < nops && ops[i].op != OP_EQ) i++;
        int end = i;

        int first_l1 = -1, last_l1 = -1, first_l2 = -1, last_l2 = -1;
        int ndel = 0, nadd = 0;
        for (int k = start; k < end; k++) {
            if (ops[k].op == OP_DEL) {
                ndel++;
                if (first_l1 < 0) first_l1 = ops[k].l1;
                last_l1 = ops[k].l1;
            } else {
                nadd++;
                if (first_l2 < 0) first_l2 = ops[k].l2;
                last_l2 = ops[k].l2;
            }
        }

        /* Hunk header */
        if (ndel > 0 && nadd > 0) {
            if (ndel == 1) printf("%d", first_l1 + 1);
            else           printf("%d,%d", first_l1+1, last_l1+1);
            putchar('c');
            if (nadd == 1) printf("%d", first_l2 + 1);
            else           printf("%d,%d", first_l2+1, last_l2+1);
        } else if (ndel > 0) {
            if (ndel == 1) printf("%d", first_l1 + 1);
            else           printf("%d,%d", first_l1+1, last_l1+1);
            /* find preceding file2 position */
            int prev_l2 = 0;
            for (int k = start-1; k >= 0; k--)
                if (ops[k].op == OP_EQ) { prev_l2 = ops[k].l2+1; break; }
            printf("d%d", prev_l2);
        } else {
            int prev_l1 = 0;
            for (int k = start-1; k >= 0; k--)
                if (ops[k].op == OP_EQ) { prev_l1 = ops[k].l1+1; break; }
            printf("%da", prev_l1);
            if (nadd == 1) printf("%d", first_l2 + 1);
            else           printf("%d,%d", first_l2+1, last_l2+1);
        }
        putchar('\n');

        for (int k = start; k < end; k++)
            if (ops[k].op == OP_DEL) printf("< %s\n", A->v[ops[k].l1]);
        if (ndel > 0 && nadd > 0) printf("---\n");
        for (int k = start; k < end; k++)
            if (ops[k].op == OP_ADD) printf("> %s\n", B->v[ops[k].l2]);
    }
}

/* ── Unified diff output ──────────────────────────────────────────────── */

static void emit_unified(const Edit *ops, int nops,
                          const Lines *A, const Lines *B,
                          const char *path1, const char *path2)
{
    printf("--- %s\n", path1);
    printf("+++ %s\n", path2);

    /* Mark ops that belong to a hunk (within CONTEXT_LINES of a change) */
    char *in_hunk = calloc((size_t)nops, 1);
    if (!in_hunk) return;
    for (int k = 0; k < nops; k++) {
        if (ops[k].op == OP_EQ) continue;
        for (int c = k - CONTEXT_LINES; c <= k + CONTEXT_LINES; c++) {
            if (c >= 0 && c < nops) in_hunk[c] = 1;
        }
    }

    int i = 0;
    while (i < nops) {
        if (!in_hunk[i]) { i++; continue; }
        int hs = i;
        while (i < nops && in_hunk[i]) i++;
        int he = i;

        /* Compute @@ header counts */
        int l1_start = -1, l2_start = -1, l1_count = 0, l2_count = 0;
        for (int k = hs; k < he; k++) {
            if (ops[k].op == OP_EQ || ops[k].op == OP_DEL) {
                if (l1_start < 0) l1_start = ops[k].l1;
                l1_count++;
            }
            if (ops[k].op == OP_EQ || ops[k].op == OP_ADD) {
                if (l2_start < 0) l2_start = ops[k].l2;
                l2_count++;
            }
        }
        if (l1_start < 0) l1_start = 0;
        if (l2_start < 0) l2_start = 0;

        printf("@@ -%d,%d +%d,%d @@\n",
               l1_start+1, l1_count, l2_start+1, l2_count);

        for (int k = hs; k < he; k++) {
            switch (ops[k].op) {
                case OP_EQ:  printf(" %s\n", A->v[ops[k].l1]); break;
                case OP_DEL: printf("-%s\n", A->v[ops[k].l1]); break;
                case OP_ADD: printf("+%s\n", B->v[ops[k].l2]); break;
            }
        }
    }
    free(in_hunk);
}

/* ── Argtable ─────────────────────────────────────────────────────────── */

static void build_diff_argtable(arg_lit_t  **help,
                                 arg_lit_t  **help_json,
                                 arg_lit_t  **unified,
                                 arg_lit_t  **ignore_case,
                                 arg_lit_t  **brief,
                                 arg_lit_t  **ignore_space,
                                 arg_file_t **files,
                                 arg_end_t  **end,
                                 void       ***argtable_out)
{
    *help         = arg_lit0("h", "help",                "show help and exit");
    *help_json    = arg_lit0(NULL, "help-json",          "print machine-readable help as JSON");
    *unified      = arg_lit0("u", "unified",             "output unified diff (3 lines of context)");
    *ignore_case  = arg_lit0("i", "ignore-case",         "ignore case differences");
    *brief        = arg_lit0("q", "brief",               "report only whether files differ");
    *ignore_space = arg_lit0("b", "ignore-space-change", "ignore changes in whitespace amount");
    *files        = arg_filen(NULL, NULL, "FILE", 2, 2,  "FILE1 FILE2 to compare");
    *end          = arg_end(20);

    static void *argtable[9];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *unified;
    argtable[3] = *ignore_case;
    argtable[4] = *brief;
    argtable[5] = *ignore_space;
    argtable[6] = *files;
    argtable[7] = *end;
    argtable[8] = NULL;
    *argtable_out = argtable;
}

void diff_print_usage(FILE *out);
extern cmd_spec_t cmd_diff_spec;

/* ── diff_run ─────────────────────────────────────────────────────────── */

int diff_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json, *unified, *ignore_case, *brief, *ignore_space;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_diff_argtable(&help, &help_json, &unified, &ignore_case, &brief,
                        &ignore_space, &files, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 8);
        diff_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_diff_spec, argtable, stdout);
        arg_freetable(argtable, 8);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "diff");
        arg_freetable(argtable, 8);
        diff_print_usage(stdout);
        return 2;
    }

    int do_unified     = (unified->count > 0);
    int do_icase       = (ignore_case->count > 0);
    int do_brief       = (brief->count > 0);
    int do_ignsp       = (ignore_space->count > 0);
    const char *path1  = files->filename[0];
    const char *path2  = files->filename[1];

    arg_freetable(argtable, 8);

    Lines A = {0}, B = {0};
    if (read_lines(path1, &A) < 0) return 2;
    if (read_lines(path2, &B) < 0) { free_lines(&A); return 2; }

    int nops = 0;
    Edit *ops = lcs_diff(&A, &B, do_icase, do_ignsp, &nops);
    if (!ops) {
        fprintf(stderr, "diff: out of memory\n");
        free_lines(&A); free_lines(&B);
        return 2;
    }

    /* Check if files differ */
    int differ = 0;
    for (int k = 0; k < nops; k++)
        if (ops[k].op != OP_EQ) { differ = 1; break; }

    if (do_brief) {
        if (differ) printf("Files %s and %s differ\n", path1, path2);
    } else if (differ) {
        if (do_unified) emit_unified(ops, nops, &A, &B, path1, path2);
        else            emit_traditional(ops, nops, &A, &B);
    }

    free(ops);
    free_lines(&A);
    free_lines(&B);
    return differ ? 1 : 0;
}

/* ── diff_print_usage ─────────────────────────────────────────────────── */

void diff_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json, *unified, *ignore_case, *brief, *ignore_space;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_diff_argtable(&help, &help_json, &unified, &ignore_case, &brief,
                        &ignore_space, &files, &end, &argtable);
    fprintf(out, "Usage: diff ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nCompare FILE1 and FILE2 line by line using LCS algorithm.\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-30s %s\n");
    arg_freetable(argtable, 8);
}

cmd_spec_t cmd_diff_spec = {
    .name      = "diff",
    .summary   = "compare two files line by line",
    .long_help = "Compare FILE1 and FILE2 using a longest-common-subsequence algorithm. "
                 "Default output uses traditional diff format (< and > markers). "
                 "Use -u for unified format with context. "
                 "Exit code: 0 = identical, 1 = different, 2 = error.",
    .run         = diff_run,
    .print_usage = diff_print_usage,
};

void register_diff_command(void) {
    register_command(&cmd_diff_spec);
}
