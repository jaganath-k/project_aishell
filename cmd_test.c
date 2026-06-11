#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

/* ── Minimal argtable (used only for --help / --help-json display) ────── */
static void build_test_argtable(arg_lit_t **help, arg_lit_t **help_json,
                                 arg_end_t **end, void ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL,"help-json", "print machine-readable help as JSON");
    *end       = arg_end(20);

    static void *argtable[4];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *end;
    argtable[3] = NULL;
    *argtable_out = argtable;
}

void test_print_usage(FILE *out);
extern cmd_spec_t cmd_test_spec;
extern cmd_spec_t cmd_bracket_spec;

/* ── Expression parser ─────────────────────────────────────────────────
 *
 * Recursive descent over argv[]:
 *   or_expr  → and_expr  ("-o" and_expr)*
 *   and_expr → not_expr  ("-a" not_expr)*
 *   not_expr → "!" not_expr | primary
 *   primary  → unary_op ARG | ARG binary_op ARG | ARG
 *
 * Returns 1 (true) or 0 (false).  Sets *err on invalid usage.
 * ─────────────────────────────────────────────────────────────────── */
typedef struct { char **argv; int argc; int pos; int err; } Eval;

static const char *peek(Eval *e)  { return e->pos < e->argc ? e->argv[e->pos] : NULL; }
static const char *eat(Eval *e)   { return e->pos < e->argc ? e->argv[e->pos++] : NULL; }

static int eval_or(Eval *e);   /* forward */

static int eval_primary(Eval *e)
{
    const char *a = peek(e);
    if (!a) return 0;

    /* Unary file / string tests: -f -d -e -r -w -x -s -L -z -n */
    if (a[0] == '-' && a[1] && !a[2]) {
        char op = a[1];
        if (strchr("fderxwsLzn", op)) {
            eat(e);                        /* consume the flag */
            const char *arg = eat(e);
            if (!arg) { e->err = 1; return 0; }
            struct stat st;
            switch (op) {
            case 'f': return (stat(arg,  &st) == 0 && S_ISREG(st.st_mode));
            case 'd': return (stat(arg,  &st) == 0 && S_ISDIR(st.st_mode));
            case 'e': return (stat(arg,  &st) == 0);
            case 'r': return (access(arg, R_OK) == 0);
            case 'w': return (access(arg, W_OK) == 0);
            case 'x': return (access(arg, X_OK) == 0);
            case 's': return (stat(arg,  &st) == 0 && st.st_size > 0);
            case 'L': return (lstat(arg, &st) == 0 && S_ISLNK(st.st_mode));
            case 'z': return (strlen(arg) == 0);
            case 'n': return (strlen(arg) >  0);
            default:  return 0;
            }
        }
    }

    /* Consume first operand */
    eat(e);
    const char *op2 = peek(e);

    /* No operator, or next is a logical combinator → treat 'a' as string test */
    if (!op2 || strcmp(op2, "-a") == 0 || strcmp(op2, "-o") == 0) {
        return (strlen(a) > 0);
    }

    /* Consume the binary operator */
    eat(e);
    const char *b = eat(e);

    /* String comparisons */
    if (strcmp(op2, "=")  == 0 || strcmp(op2, "==") == 0)
        return b ? (strcmp(a, b) == 0) : 0;
    if (strcmp(op2, "!=") == 0)
        return b ? (strcmp(a, b) != 0) : 0;

    /* Numeric comparisons */
    if (!b) { e->err = 1; return 0; }
    long la = atol(a), lb = atol(b);
    if (strcmp(op2, "-eq") == 0) return (la == lb);
    if (strcmp(op2, "-ne") == 0) return (la != lb);
    if (strcmp(op2, "-lt") == 0) return (la <  lb);
    if (strcmp(op2, "-le") == 0) return (la <= lb);
    if (strcmp(op2, "-gt") == 0) return (la >  lb);
    if (strcmp(op2, "-ge") == 0) return (la >= lb);

    /* Unrecognised binary op: treat 'a' as non-empty string test */
    return (strlen(a) > 0);
}

static int eval_not(Eval *e)
{
    const char *a = peek(e);
    if (a && strcmp(a, "!") == 0) {
        eat(e);
        return !eval_not(e);
    }
    return eval_primary(e);
}

static int eval_and(Eval *e)
{
    int result = eval_not(e);
    while (peek(e) && strcmp(peek(e), "-a") == 0) {
        eat(e);
        int r = eval_not(e);
        result = result && r;
    }
    return result;
}

static int eval_or(Eval *e)
{
    int result = eval_and(e);
    while (peek(e) && strcmp(peek(e), "-o") == 0) {
        eat(e);
        int r = eval_and(e);
        result = result || r;
    }
    return result;
}

/* ── test_run ────────────────────────────────────────────────────────────
 * Shared by both "test" and "[" (argv[0] differs; run() detects which).
 * ─────────────────────────────────────────────────────────────────────── */
int test_run(int argc, char **argv)
{
    /* Detect which spec we're running under for --help-json */
    int is_bracket = (strcmp(argv[0], "[") == 0);

    /* Handle --help / --help-json first */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            test_print_usage(stdout); return 0;
        }
        if (strcmp(argv[i], "--help-json") == 0) {
            arg_lit_t *help, *help_json;
            arg_end_t *end;
            void     **argtable;
            build_test_argtable(&help, &help_json, &end, &argtable);
            cmd_spec_t *spec = is_bracket ? &cmd_bracket_spec : &cmd_test_spec;
            print_help_json(spec, argtable, stdout);
            arg_freetable(argtable, 3);
            return 0;
        }
    }

    /* For the "[" form the last argument MUST be "]" */
    if (is_bracket) {
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
            fprintf(stderr, "[: missing closing ]\n");
            return 2;
        }
        argc--;     /* strip the "]" */
    }

    /* No expression → false (exit 1) */
    if (argc <= 1) return 1;

    /* Evaluate expression starting at argv[1] */
    Eval ev = { .argv = argv, .argc = argc, .pos = 1, .err = 0 };
    int result = eval_or(&ev);

    if (ev.err) return 2;       /* syntax / usage error */
    return result ? 0 : 1;
}

/* ── Usage / spec ─────────────────────────────────────────────────────── */
void test_print_usage(FILE *out)
{
    fprintf(out, "Usage: test EXPR\n");
    fprintf(out, "       [ EXPR ]\n\n");
    fprintf(out, "Evaluate a conditional EXPR; exit 0 if true, 1 if false, 2 on error.\n\n");
    fprintf(out, "File tests:       -f FILE  -d FILE  -e FILE  -r FILE  -w FILE\n");
    fprintf(out, "                  -x FILE  -s FILE  -L FILE\n");
    fprintf(out, "String tests:     -z STR   -n STR\n");
    fprintf(out, "String compare:   STR1 = STR2   STR1 != STR2\n");
    fprintf(out, "Numeric compare:  N1 -eq|-ne|-lt|-le|-gt|-ge N2\n");
    fprintf(out, "Logical:          ! EXPR   EXPR -a EXPR   EXPR -o EXPR\n");
    fprintf(out, "\nOptions:\n");
    fprintf(out, "  --help        show this help\n");
    fprintf(out, "  --help-json   print machine-readable help as JSON\n");
}

cmd_spec_t cmd_test_spec = {
    .name      = "test",
    .summary   = "evaluate a conditional expression",
    .long_help = "Evaluate EXPR and return exit 0 (true) or 1 (false). "
                 "Supports file tests (-f/-d/-e/-r/-w/-x/-s/-L), string tests "
                 "(-z/-n, = !=), numeric comparisons (-eq/-ne/-lt/-le/-gt/-ge), "
                 "and logical operators (! -a -o).",
    .run         = test_run,
    .print_usage = test_print_usage,
};

cmd_spec_t cmd_bracket_spec = {
    .name      = "[",
    .summary   = "evaluate a conditional expression (alias for test)",
    .long_help = "Same as test but requires a closing ] as the last argument. "
                 "Used in: if [ -f file ]; then ... fi",
    .run         = test_run,
    .print_usage = test_print_usage,
};

void register_test_command(void)    { register_command(&cmd_test_spec); }
void register_bracket_command(void) { register_command(&cmd_bracket_spec); }
