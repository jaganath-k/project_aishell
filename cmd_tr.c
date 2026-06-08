#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

static void build_tr_argtable(arg_lit_t **help,
                               arg_lit_t **help_json,
                               arg_lit_t **delete_flag,
                               arg_lit_t **squeeze,
                               arg_lit_t **complement,
                               arg_str_t **sets,
                               arg_end_t **end,
                               void      ***argtable_out)
{
    *help        = arg_lit0("h", "help",            "show help and exit");
    *help_json   = arg_lit0(NULL, "help-json",      "print machine-readable help as JSON");
    *delete_flag = arg_lit0("d", "delete",          "delete characters in SET1 from input");
    *squeeze     = arg_lit0("s", "squeeze-repeats", "squeeze repeated SET1 chars to one");
    *complement  = arg_lit0("c", "complement",      "use complement of SET1");
    *sets        = arg_strn(NULL, NULL, "SET", 1, 2, "SET1 [SET2]");
    *end         = arg_end(20);

    static void *argtable[8];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *delete_flag;
    argtable[3] = *squeeze;
    argtable[4] = *complement;
    argtable[5] = *sets;
    argtable[6] = *end;
    argtable[7] = NULL;
    *argtable_out = argtable;
}

/* Expand a tr SET spec into an ordered byte array; returns count or -1. */
static int expand_set(const char *spec, unsigned char out[256]) {
    int n = 0;
    const char *p = spec;
    while (*p && n < 256) {
        if (p[0] == '[' && p[1] == ':') {
            /* POSIX named class [:upper:] etc. */
            const char *close = strstr(p + 2, ":]");
            if (!close) return -1;
            size_t nlen = (size_t)(close - p - 2);
            char name[32];
            if (nlen >= sizeof(name)) return -1;
            memcpy(name, p + 2, nlen);
            name[nlen] = '\0';
            for (int c = 0; c < 256 && n < 256; c++) {
                int match = 0;
                if      (strcmp(name, "upper") == 0) match = isupper(c);
                else if (strcmp(name, "lower") == 0) match = islower(c);
                else if (strcmp(name, "digit") == 0) match = isdigit(c);
                else if (strcmp(name, "space") == 0) match = isspace(c);
                else if (strcmp(name, "alpha") == 0) match = isalpha(c);
                else if (strcmp(name, "alnum") == 0) match = isalnum(c);
                else if (strcmp(name, "print") == 0) match = isprint(c);
                else if (strcmp(name, "punct") == 0) match = ispunct(c);
                if (match) out[n++] = (unsigned char)c;
            }
            p = close + 2;
        } else if (p[0] == '\\' && p[1]) {
            /* C-style escape: \n \t \r \a \b \\ */
            unsigned char c;
            switch (p[1]) {
                case 'n':  c = '\n'; break;
                case 't':  c = '\t'; break;
                case 'r':  c = '\r'; break;
                case 'a':  c = '\a'; break;
                case 'b':  c = '\b'; break;
                case '\\': c = '\\'; break;
                default:   c = (unsigned char)p[1]; break;
            }
            out[n++] = c;
            p += 2;
        } else if (p[1] == '-' && p[2]) {
            /* Range: lo-hi */
            unsigned char lo = (unsigned char)p[0];
            unsigned char hi = (unsigned char)p[2];
            if (lo <= hi) {
                for (unsigned int c = lo; c <= (unsigned int)hi && n < 256; c++)
                    out[n++] = (unsigned char)c;
            } else {
                out[n++] = lo;   /* invalid range: treat as literal */
            }
            p += 3;
        } else {
            out[n++] = (unsigned char)*p++;
        }
    }
    return n;
}

static void run_tr(int do_delete, int do_squeeze, int do_complement,
                   const unsigned char *s1, int n1,
                   const unsigned char *s2, int n2)
{
    /* Build SET1 bitmap, then optionally complement it */
    unsigned char in1[256] = {0};
    for (int i = 0; i < n1; i++) in1[(unsigned char)s1[i]] = 1;
    if (do_complement)
        for (int c = 0; c < 256; c++) in1[c] = !in1[c];

    /* Build translation table (identity by default) */
    unsigned char xlat[256];
    for (int c = 0; c < 256; c++) xlat[c] = (unsigned char)c;

    if (!do_delete && n2 > 0) {
        if (!do_complement) {
            /* Map SET1[i] → SET2[i]; last SET2 char repeats for overflow */
            for (int i = 0; i < n1; i++) {
                int j = (i < n2) ? i : n2 - 1;
                xlat[(unsigned char)s1[i]] = s2[j];
            }
        } else {
            /* Complement translate: map each char not in original SET1 to SET2 */
            int j = 0;
            for (int c = 0; c < 256; c++) {
                if (in1[c]) {   /* in1 is already the complement */
                    xlat[c] = (unsigned char)((j < n2) ? s2[j] : s2[n2 - 1]);
                    j++;
                }
            }
        }
    }

    /* Squeeze bitmap: chars to collapse after translation */
    unsigned char in_sq[256] = {0};
    if (do_squeeze) {
        if (!do_delete && n2 > 0)
            /* Squeeze on translated output chars (SET2) */
            for (int i = 0; i < n2; i++) in_sq[(unsigned char)s2[i]] = 1;
        else
            /* Squeeze on SET1 (or its complement) */
            memcpy(in_sq, in1, 256);
    }

    int last_out = -1;
    int c;
    while ((c = getchar()) != EOF) {
        if (do_delete && in1[(unsigned char)c]) continue;
        unsigned char out = xlat[(unsigned char)c];
        if (do_squeeze && in_sq[(unsigned char)out] && (int)out == last_out) continue;
        putchar(out);
        last_out = (int)out;
    }
}

void tr_print_usage(FILE *out);
extern cmd_spec_t cmd_tr_spec;

int tr_run(int argc, char **argv) {
    arg_lit_t *help, *help_json, *delete_flag, *squeeze, *complement;
    arg_str_t *sets;
    arg_end_t *end;
    void     **argtable;

    build_tr_argtable(&help, &help_json, &delete_flag, &squeeze, &complement,
                      &sets, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 7);
        tr_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_tr_spec, argtable, stdout);
        arg_freetable(argtable, 7);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "tr");
        arg_freetable(argtable, 7);
        tr_print_usage(stdout);
        return 1;
    }

    int do_delete     = (delete_flag->count > 0);
    int do_squeeze    = (squeeze->count > 0);
    int do_complement = (complement->count > 0);

    /* SET2 is required for translate (no -d, no -s alone) */
    if (!do_delete && !do_squeeze && sets->count < 2) {
        fprintf(stderr, "tr: missing operand — SET2 required for translation\n");
        arg_freetable(argtable, 7);
        tr_print_usage(stdout);
        return 1;
    }

    unsigned char s1[256], s2[256];
    int n1 = expand_set(sets->sval[0], s1);
    int n2 = (sets->count > 1) ? expand_set(sets->sval[1], s2) : 0;

    if (n1 < 0) {
        fprintf(stderr, "tr: invalid SET1 specification\n");
        arg_freetable(argtable, 7);
        return 1;
    }
    if (n2 < 0) {
        fprintf(stderr, "tr: invalid SET2 specification\n");
        arg_freetable(argtable, 7);
        return 1;
    }

    arg_freetable(argtable, 7);
    run_tr(do_delete, do_squeeze, do_complement, s1, n1, s2, n2);
    return 0;
}

void tr_print_usage(FILE *out) {
    arg_lit_t *help, *help_json, *delete_flag, *squeeze, *complement;
    arg_str_t *sets;
    arg_end_t *end;
    void     **argtable;

    build_tr_argtable(&help, &help_json, &delete_flag, &squeeze, &complement,
                      &sets, &end, &argtable);
    fprintf(out, "Usage: tr ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nTranslate, squeeze, or delete characters from stdin to stdout.\n");
    fprintf(out, "\nSET syntax: literal chars, ranges (a-z), POSIX classes ([:upper:] etc.), escapes (\\n \\t \\r).\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-28s %s\n");
    arg_freetable(argtable, 7);
}

cmd_spec_t cmd_tr_spec = {
    .name      = "tr",
    .summary   = "translate or delete characters",
    .long_help = "Translate, squeeze, or delete characters read from stdin, writing to stdout. "
                 "SET1 and SET2 support literal chars, ranges (a-z, A-Z, 0-9), "
                 "POSIX classes ([:upper:] [:lower:] [:digit:] [:space:] [:alpha:]), "
                 "and escape sequences (\\n \\t \\r). "
                 "tr takes no file arguments — always reads stdin.",
    .run         = tr_run,
    .print_usage = tr_print_usage,
};

void register_tr_command(void) {
    register_command(&cmd_tr_spec);
}
