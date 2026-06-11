#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/time.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

/* ── Argtable ─────────────────────────────────────────────────────────── */
static void build_read_argtable(
        arg_lit_t **help,    arg_lit_t **help_json,
        arg_str_t **prompt,  arg_lit_t **silent,
        arg_int_t **nchars,  arg_str_t **delim,
        arg_int_t **timeout_arg,
        arg_str_t **var_name,
        arg_end_t **end,
        void ***argtable_out)
{
    *help        = arg_lit0("h", "help",       "show help and exit");
    *help_json   = arg_lit0(NULL,"help-json",  "print machine-readable help as JSON");
    *prompt      = arg_str0("p", "prompt",  "PROMPT", "print PROMPT to stderr before reading");
    *silent      = arg_lit0("s", "silent",           "do not echo input characters");
    *nchars      = arg_int0("n", "nchars",  "N",     "return after reading N characters");
    *delim       = arg_str0("d", "delimiter","DELIM", "stop at DELIM instead of newline");
    *timeout_arg = arg_int0("t", "timeout", "N",     "timeout after N seconds (exit 1)");
    *var_name    = arg_str1(NULL, NULL,     "VAR",   "environment variable to assign");
    *end         = arg_end(20);

    static void *argtable[10];
    argtable[0] = *help;         argtable[1] = *help_json;
    argtable[2] = *prompt;       argtable[3] = *silent;
    argtable[4] = *nchars;       argtable[5] = *delim;
    argtable[6] = *timeout_arg;  argtable[7] = *var_name;
    argtable[8] = *end;          argtable[9] = NULL;
    *argtable_out = argtable;
}

void read_print_usage(FILE *out);
extern cmd_spec_t cmd_read_spec;

/* ── read_run ────────────────────────────────────────────────────────────── */
int read_run(int argc, char **argv)
{
    arg_lit_t *help, *help_json, *silent;
    arg_str_t *prompt, *delim, *var_name;
    arg_int_t *nchars, *timeout_arg;
    arg_end_t *end;
    void     **argtable;

    build_read_argtable(&help, &help_json, &prompt, &silent, &nchars, &delim,
                        &timeout_arg, &var_name, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 9); read_print_usage(stdout); return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_read_spec, argtable, stdout);
        arg_freetable(argtable, 9); return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "read");
        arg_freetable(argtable, 9); read_print_usage(stdout); return 2;
    }

    /* Extract options */
    const char *prompt_str = prompt->count      > 0 ? prompt->sval[0]       : NULL;
    int   do_silent         = (silent->count     > 0);
    int   n_ch              = nchars->count      > 0 ? nchars->ival[0]       : 0;
    char  stop_c            = delim->count       > 0 ? delim->sval[0][0]     : '\n';
    int   timeout_s         = timeout_arg->count > 0 ? timeout_arg->ival[0]  : 0;

    char varname[256] = "";
    strncpy(varname, var_name->sval[0], sizeof(varname) - 1);
    arg_freetable(argtable, 9);

    /* Print prompt to stderr */
    if (prompt_str) { fputs(prompt_str, stderr); fflush(stderr); }

    /* Terminal modifications: silent (-s) and/or raw mode (-n N) */
    struct termios old_tio;
    int tty_changed = 0;
    int is_tty      = isatty(STDIN_FILENO);

    if (is_tty && (do_silent || n_ch > 0)) {
        if (tcgetattr(STDIN_FILENO, &old_tio) == 0) {
            struct termios new_tio = old_tio;
            if (n_ch > 0) {
                /* Raw mode: each keypress delivered immediately without Enter */
                new_tio.c_lflag &= ~(ICANON | ISIG);
                new_tio.c_cc[VMIN]  = 1;
                new_tio.c_cc[VTIME] = 0;
            }
            if (do_silent) {
                new_tio.c_lflag &= ~ECHO;
            }
            if (tcsetattr(STDIN_FILENO, TCSANOW, &new_tio) == 0)
                tty_changed = 1;
        }
    }

    /* Timeout: wait up to timeout_s seconds for any input */
    if (timeout_s > 0) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = { .tv_sec = timeout_s, .tv_usec = 0 };
        int r = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
        if (r <= 0) {
            /* Timed out or error */
            if (tty_changed) tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_tio);
            return 1;
        }
    }

    /* Read input character by character */
    char buf[4096];
    int  len       = 0;
    int  got_delim = 0;

    for (;;) {
        if (n_ch > 0 && len >= n_ch) break;       /* -n N limit reached */
        if (len >= (int)(sizeof(buf) - 1)) break;  /* buffer full */

        int c = fgetc(stdin);
        if (c == EOF) break;

        /* In raw mode on a TTY, Enter sends '\r' — treat as line end */
        if (is_tty && n_ch == 0 && (char)c == '\r') { got_delim = 1; break; }
        if ((char)c == stop_c)                       { got_delim = 1; break; }

        buf[len++] = (char)c;
    }
    buf[len] = '\0';

    /* Restore terminal */
    if (tty_changed) tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_tio);

    /* Assign to environment variable */
    setenv(varname, buf, 1);

    /* Return 1 (failure) if no data was read at all (EOF with no delimiter) */
    return (len == 0 && !got_delim) ? 1 : 0;
}

/* ── Usage / spec ─────────────────────────────────────────────────────── */
void read_print_usage(FILE *out)
{
    arg_lit_t *help, *help_json, *silent;
    arg_str_t *prompt, *delim, *var_name;
    arg_int_t *nchars, *timeout_arg;
    arg_end_t *end;
    void     **argtable;

    build_read_argtable(&help, &help_json, &prompt, &silent, &nchars, &delim,
                        &timeout_arg, &var_name, &end, &argtable);
    fprintf(out, "Usage: read ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nRead a line from stdin and assign it to environment variable VAR.\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-28s %s\n");
    arg_freetable(argtable, 9);
}

cmd_spec_t cmd_read_spec = {
    .name      = "read",
    .summary   = "read a line from stdin into a variable",
    .long_help = "Read one line from standard input and assign the value to "
                 "the environment variable VAR via setenv(). Supports silent "
                 "input (-s, useful for passwords), character-count limit "
                 "(-n N enters raw mode so no Enter needed), custom stop "
                 "delimiter (-d DELIM), and a timeout (-t N seconds, returns "
                 "1 if no input arrives in time).",
    .run         = read_run,
    .print_usage = read_print_usage,
};

void register_read_command(void) {
    register_command(&cmd_read_spec);
}
