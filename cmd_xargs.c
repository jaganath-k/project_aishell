#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

#define XARGS_MAX_ITEMS 65536

/* ── String helpers ────────────────────────────────────────────────────── */

/* Replace every occurrence of old_s in src with new_s.  Caller must free. */
static char *str_replace_all(const char *src, const char *old_s, const char *new_s)
{
    size_t olen = strlen(old_s);
    size_t nlen = strlen(new_s);
    if (olen == 0) return strdup(src);

    int count = 0;
    const char *q = src;
    while ((q = strstr(q, old_s)) != NULL) { count++; q += olen; }
    if (count == 0) return strdup(src);

    size_t srclen = strlen(src);
    long   delta  = (long)nlen - (long)olen;
    size_t newlen = (size_t)((long)srclen + (long)count * delta) + 1;
    char  *res    = malloc(newlen);
    if (!res) return NULL;

    char       *dst = res;
    const char *p   = src, *found;
    while ((found = strstr(p, old_s)) != NULL) {
        size_t pre = (size_t)(found - p);
        memcpy(dst, p, pre); dst += pre;
        memcpy(dst, new_s, nlen); dst += nlen;
        p = found + olen;
    }
    strcpy(dst, p);
    return res;
}

/* ── Read all of stdin into a heap buffer ──────────────────────────────── */
static char *read_stdin_all(size_t *out_len)
{
    size_t cap = 4096, len = 0;
    char  *buf = malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        ssize_t r = read(STDIN_FILENO, buf + len, cap - len - 1);
        if (r < 0) { if (errno == EINTR) continue; free(buf); return NULL; }
        if (r == 0) break;
        len += (size_t)r;
    }
    buf[len] = '\0';
    *out_len  = len;
    return buf;
}

/* ── Split buf into item pointers (modifies buf for whitespace terminators) */
static int split_items(char *buf, size_t buflen,
                       int use_null, int use_delim, char delim_c,
                       char **items, int max_items)
{
    int    n   = 0;
    char  *p   = buf;
    char  *end = buf + buflen;

    if (use_null) {
        while (p < end && n < max_items) {
            char *start = p;
            while (p < end && *p != '\0') p++;
            if (p > start) items[n++] = start;
            if (p < end) p++;          /* skip the NUL separator */
        }
    } else if (use_delim) {
        char *start = p;
        while (p < end && n < max_items) {
            if (*p == delim_c) {
                *p = '\0';
                if (start < p) items[n++] = start;
                start = p + 1;
            }
            p++;
        }
        if (start < end && *start && n < max_items)
            items[n++] = start;
    } else {
        /* Default: split on whitespace */
        while (p < end && n < max_items) {
            while (p < end && isspace((unsigned char)*p)) p++;
            if (p >= end) break;
            char *start = p;
            while (p < end && !isspace((unsigned char)*p)) p++;
            if (p < end) *p++ = '\0';
            items[n++] = start;
        }
    }
    return n;
}

/* ── Lightweight process pool for -P N ─────────────────────────────────── */
typedef struct { pid_t *pids; int max, cur, rc; } Pool;

static void pool_record(Pool *p, int status)
{
    if (!WIFEXITED(status)) { if (!p->rc) p->rc = 1; return; }
    int ec = WEXITSTATUS(status);
    if      (ec == 255)    p->rc = 123;
    else if (ec && !p->rc) p->rc = 1;
}

/* Wait until there is a free slot in the pool. */
static void pool_wait_slot(Pool *p)
{
    /* First: reap any already-finished children without blocking */
    int st; pid_t done;
    while ((done = waitpid(-1, &st, WNOHANG)) > 0) {
        for (int i = 0; i < p->max; i++)
            if (p->pids[i] == done) { p->pids[i] = 0; p->cur--; break; }
        pool_record(p, st);
    }
    /* Then: block if still full */
    while (p->cur >= p->max) {
        done = waitpid(-1, &st, 0);
        if (done > 0) {
            for (int i = 0; i < p->max; i++)
                if (p->pids[i] == done) { p->pids[i] = 0; p->cur--; break; }
            pool_record(p, st);
        }
    }
}

static void pool_add(Pool *p, pid_t pid)
{
    for (int i = 0; i < p->max; i++)
        if (p->pids[i] == 0) { p->pids[i] = pid; p->cur++; return; }
}

static int pool_drain(Pool *p)
{
    while (p->cur > 0) {
        int st; pid_t done = waitpid(-1, &st, 0);
        if (done > 0) {
            for (int i = 0; i < p->max; i++)
                if (p->pids[i] == done) { p->pids[i] = 0; p->cur--; break; }
            pool_record(p, st);
        } else break;
    }
    return p->rc;
}

/* ── Spawn helpers ───────────────────────────────────────────────────────── */

static pid_t exec_argv(char **argv)
{
    pid_t pid = fork();
    if (pid == 0) { execvp(argv[0], argv); _exit(127); }
    return pid;
}

/* Spawn base[0..bn-1] + extra[0..en-1] */
static pid_t spawn_batch(char **base, int bn, char **extra, int en)
{
    char **argv = malloc((size_t)(bn + en + 1) * sizeof(char *));
    if (!argv) return -1;
    for (int i = 0; i < bn; i++) argv[i]      = base[i];
    for (int i = 0; i < en; i++) argv[bn + i]  = extra[i];
    argv[bn + en] = NULL;
    pid_t pid = exec_argv(argv);
    free(argv);
    return pid;
}

/* Spawn base[0..bn-1] with rep replaced by item in every arg string */
static pid_t spawn_replace(char **base, int bn,
                            const char *rep, const char *item)
{
    char **argv  = malloc((size_t)(bn + 1) * sizeof(char *));
    char **freed = calloc((size_t)bn,       sizeof(char *));
    if (!argv || !freed) { free(argv); free(freed); return -1; }

    for (int i = 0; i < bn; i++) {
        if (strstr(base[i], rep)) {
            argv[i]  = str_replace_all(base[i], rep, item);
            freed[i] = argv[i];
        } else {
            argv[i] = base[i];
        }
    }
    argv[bn] = NULL;
    pid_t pid = exec_argv(argv);
    for (int i = 0; i < bn; i++) free(freed[i]);
    free(freed);
    free(argv);
    return pid;
}

/* ── Manual option parser ────────────────────────────────────────────────
 * argtable3 is unsuitable for meta-commands like xargs because it treats
 * any unrecognised '-x' token as an error — but CMD args like 'wc -l'
 * contain flags that belong to CMD, not xargs.  We therefore scan options
 * by hand and let everything after the last xargs flag become CMD + ARGS.
 * argtable3 is still used for --help / --help-json display only.
 * ─────────────────────────────────────────────────────────────────────── */
typedef struct {
    char *rep;       /* -I REPLACE (points into argv, not owned) */
    int   npercmd;   /* -n N  (0 = unlimited)  */
    int   nprocs;    /* -P N  (default 1)       */
    char  delim_c;   /* -d DELIM first char     */
    int   use_delim;
    int   do_null;   /* -0 / --null             */
    int   cmd_start; /* index of first CMD arg in argv */
} XargsOpts;

static int parse_xargs_opts(int argc, char **argv, XargsOpts *o)
{
    o->rep       = NULL;
    o->npercmd   = 0;
    o->nprocs    = 1;
    o->delim_c   = '\0';
    o->use_delim = 0;
    o->do_null   = 0;
    o->cmd_start = argc; /* default: no CMD */

    int i = 1;
    while (i < argc) {
        char *a = argv[i];

        if (a[0] != '-' || strcmp(a, "-") == 0) {
            /* First non-option token starts the CMD */
            o->cmd_start = i;
            return 0;
        }
        if (strcmp(a, "--") == 0) { o->cmd_start = i + 1; return 0; }

        /* -0 / --null */
        if (strcmp(a, "-0") == 0 || strcmp(a, "--null") == 0) {
            o->do_null = 1; i++; continue;
        }
        /* -I [REPLACE] or -IREPLACE */
        if (strcmp(a, "-I") == 0 || strcmp(a, "--replace") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "xargs: option requires an argument: %s\n", a);
                return -1;
            }
            o->rep = argv[++i]; i++; continue;
        }
        if (strncmp(a, "-I", 2) == 0 && a[2]) {
            o->rep = a + 2; i++; continue;
        }
        if (strncmp(a, "--replace=", 10) == 0) {
            o->rep = a + 10; i++; continue;
        }
        /* -n N / --max-args=N */
        if (strcmp(a, "-n") == 0 || strcmp(a, "--max-args") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "xargs: option requires an argument: %s\n", a);
                return -1;
            }
            o->npercmd = atoi(argv[++i]); i++; continue;
        }
        if (strncmp(a, "-n", 2) == 0 && isdigit((unsigned char)a[2])) {
            o->npercmd = atoi(a + 2); i++; continue;
        }
        if (strncmp(a, "--max-args=", 11) == 0) {
            o->npercmd = atoi(a + 11); i++; continue;
        }
        /* -P N / --max-procs=N */
        if (strcmp(a, "-P") == 0 || strcmp(a, "--max-procs") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "xargs: option requires an argument: %s\n", a);
                return -1;
            }
            o->nprocs = atoi(argv[++i]); i++; continue;
        }
        if (strncmp(a, "-P", 2) == 0 && isdigit((unsigned char)a[2])) {
            o->nprocs = atoi(a + 2); i++; continue;
        }
        if (strncmp(a, "--max-procs=", 12) == 0) {
            o->nprocs = atoi(a + 12); i++; continue;
        }
        /* -d DELIM / --delimiter=DELIM */
        if (strcmp(a, "-d") == 0 || strcmp(a, "--delimiter") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "xargs: option requires an argument: %s\n", a);
                return -1;
            }
            o->delim_c   = argv[++i][0];
            o->use_delim = 1; i++; continue;
        }
        if (strncmp(a, "-d", 2) == 0 && a[2]) {
            o->delim_c = a[2]; o->use_delim = 1; i++; continue;
        }
        if (strncmp(a, "--delimiter=", 12) == 0) {
            o->delim_c = a[12]; o->use_delim = 1; i++; continue;
        }

        /* Unknown option: treat as start of CMD (forward-compatible) */
        o->cmd_start = i;
        return 0;
    }
    o->cmd_start = argc; /* all tokens were xargs flags; no CMD given */
    return 0;
}

/* ── Argtable (used only by print_usage / --help-json) ──────────────────── */
static void build_xargs_argtable(
        arg_lit_t **help,    arg_lit_t **help_json,
        arg_str_t **replace, arg_int_t **max_args,
        arg_int_t **max_procs, arg_str_t **delim,
        arg_lit_t **null_term, arg_str_t **cmd_args,
        arg_end_t **end,
        void ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL,"help-json", "print machine-readable help as JSON");
    *replace   = arg_str0("I", "replace",   "REPLACE",
                          "replace REPLACE in CMD with each input item");
    *max_args  = arg_int0("n", "max-args",  "N",
                          "use at most N arguments per command invocation");
    *max_procs = arg_int0("P", "max-procs", "N",
                          "run up to N processes simultaneously (default 1)");
    *delim     = arg_str0("d", "delimiter", "DELIM",
                          "input item delimiter (default: whitespace)");
    *null_term = arg_lit0("0", "null",
                          "input items are NUL-terminated (for find -print0)");
    *cmd_args  = arg_strn(NULL, NULL, "CMD", 0, 1024,
                          "command to run with args (default: echo)");
    *end       = arg_end(20);

    static void *argtable[10];
    argtable[0] = *help;       argtable[1] = *help_json;
    argtable[2] = *replace;    argtable[3] = *max_args;
    argtable[4] = *max_procs;  argtable[5] = *delim;
    argtable[6] = *null_term;  argtable[7] = *cmd_args;
    argtable[8] = *end;        argtable[9] = NULL;
    *argtable_out = argtable;
}

void xargs_print_usage(FILE *out);
extern cmd_spec_t cmd_xargs_spec;

/* ── xargs_run ──────────────────────────────────────────────────────────── */
int xargs_run(int argc, char **argv)
{
    /* Handle --help / --help-json before option scanning */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            xargs_print_usage(stdout); return 0;
        }
        if (strcmp(argv[i], "--help-json") == 0) {
            arg_lit_t *help, *help_json, *null_term;
            arg_str_t *replace, *delim, *cmd_args;
            arg_int_t *max_args, *max_procs;
            arg_end_t *end;
            void     **argtable;
            build_xargs_argtable(&help, &help_json, &replace, &max_args,
                                  &max_procs, &delim, &null_term, &cmd_args,
                                  &end, &argtable);
            print_help_json(&cmd_xargs_spec, argtable, stdout);
            arg_freetable(argtable, 9);
            return 0;
        }
    }

    /* Manual option parsing (argtable3 can't handle CMD flags like 'wc -l') */
    XargsOpts o;
    if (parse_xargs_opts(argc, argv, &o) < 0) {
        xargs_print_usage(stderr); return 125;
    }

    /* CMD and its initial args are argv[cmd_start..argc-1] */
    char *echo_default[] = {"echo", NULL};
    char **base_argv;
    int   base_argc;
    if (o.cmd_start < argc) {
        base_argv = argv + o.cmd_start;
        base_argc = argc - o.cmd_start;
    } else {
        base_argv = echo_default;
        base_argc = 1;
    }

    /* Read stdin */
    size_t  buflen = 0;
    char   *inbuf  = read_stdin_all(&buflen);
    if (!inbuf) return 125;

    char **items = malloc(XARGS_MAX_ITEMS * sizeof(char *));
    if (!items) { free(inbuf); return 125; }

    int n_items = split_items(inbuf, buflen, o.do_null, o.use_delim, o.delim_c,
                               items, XARGS_MAX_ITEMS);
    if (n_items == 0) { free(items); free(inbuf); return 0; }

    /* Set up process pool */
    int nprocs = o.nprocs > 0 ? o.nprocs : 1;
    Pool pool  = {
        .pids = calloc((size_t)nprocs, sizeof(pid_t)),
        .max  = nprocs, .cur = 0, .rc = 0
    };
    if (!pool.pids) { free(items); free(inbuf); return 125; }

    int overall_rc = 0;

    if (o.rep) {
        /* -I mode: one invocation per item with placeholder replaced */
        for (int i = 0; i < n_items && pool.rc != 123; i++) {
            pool_wait_slot(&pool);
            pid_t pid = spawn_replace(base_argv, base_argc, o.rep, items[i]);
            if (pid < 0) { overall_rc = 125; break; }
            pool_add(&pool, pid);
        }
    } else {
        /* Batch mode: group items into chunks of npercmd (0 = all at once) */
        int batch = (o.npercmd > 0) ? o.npercmd : n_items;
        for (int i = 0; i < n_items && pool.rc != 123; i += batch) {
            int this_n = (i + batch <= n_items) ? batch : n_items - i;
            pool_wait_slot(&pool);
            pid_t pid = spawn_batch(base_argv, base_argc, items + i, this_n);
            if (pid < 0) { overall_rc = 125; break; }
            pool_add(&pool, pid);
        }
    }

    int drain_rc = pool_drain(&pool);
    if (!overall_rc) overall_rc = drain_rc;

    free(pool.pids);
    free(items);
    free(inbuf);
    return overall_rc;
}

/* ── Usage / spec ─────────────────────────────────────────────────────── */
void xargs_print_usage(FILE *out)
{
    arg_lit_t *help, *help_json, *null_term;
    arg_str_t *replace, *delim, *cmd_args;
    arg_int_t *max_args, *max_procs;
    arg_end_t *end;
    void     **argtable;

    build_xargs_argtable(&help, &help_json, &replace, &max_args, &max_procs,
                         &delim, &null_term, &cmd_args, &end, &argtable);
    fprintf(out, "Usage: xargs ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nBuild and execute command lines from standard input.\n");
    fprintf(out, "  echo 'a b c' | xargs echo       run 'echo a b c'\n");
    fprintf(out, "  ... | xargs -n 1 CMD             one item per invocation\n");
    fprintf(out, "  find . -print0 | xargs -0 CMD    handle spaces in names\n");
    fprintf(out, "  ... | xargs -I{} CMD arg {} arg  replace {} with each item\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-32s %s\n");
    arg_freetable(argtable, 9);
}

cmd_spec_t cmd_xargs_spec = {
    .name      = "xargs",
    .summary   = "build and execute command lines from stdin",
    .long_help = "Read items from stdin and execute CMD with those items as "
                 "arguments. Items are split on whitespace by default. Use "
                 "-0 for NUL-delimited input (find -print0), -d DELIM for a "
                 "custom delimiter, -n N to limit args per invocation, "
                 "-I REPLACE for one-at-a-time placeholder substitution, "
                 "and -P N for parallel execution.",
    .run         = xargs_run,
    .print_usage = xargs_print_usage,
};

void register_xargs_command(void) {
    register_command(&cmd_xargs_spec);
}
