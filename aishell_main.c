#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <glob.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "cmd_spec.h"

/* =========================================================================
 * Constants  (mirrors Token.h / Command.h from the reference)
 * ===================================================================== */
#define MAX_NUM_TOKENS    1000
#define MAX_NUM_COMMANDS  100
#define TOKEN_SEPARATORS  " \t\r\n\a"

/* =========================================================================
 * Command structure  (mirrors Command.h)
 *
 *   first / last  — inclusive indices into the flat token[] array
 *   sep           — separator that follows the command (always ";")
 *   argv / argc   — glob-expanded argument vector (heap-allocated)
 *   stdin_file    — filename from '<' redirection (points into token[])
 *   stdout_file   — filename from '>' redirection (points into token[])
 * ===================================================================== */
typedef struct {
    int    first;
    int    last;
    char  *sep;
    char **argv;
    int    argc;
    char  *stdin_file;
    char  *stdout_file;
} cmd_t;

/* =========================================================================
 * Forward declarations of all register functions
 * ===================================================================== */
extern void register_ls_command(void);
extern void register_cat_command(void);
extern void register_stat_command(void);
extern void register_head_command(void);
extern void register_tail_command(void);
extern void register_cp_command(void);
extern void register_mv_command(void);
extern void register_rm_command(void);
extern void register_mkdir_command(void);
extern void register_rmdir_command(void);
extern void register_touch_command(void);
extern void register_rg_command(void);
extern void register_ps_command(void);
extern void register_kill_command(void);
extern void register_wait_command(void);
extern void register_jobs_command(void);
extern void register_cd_command(void);
extern void register_pwd_command(void);
extern void register_echo_command(void);
extern void register_env_command(void);
extern void register_export_command(void);
extern void register_unset_command(void);
extern void register_type_command(void);
extern void register_edit_replace_line_command(void);
extern void register_edit_insert_line_command(void);
extern void register_edit_delete_line_command(void);
extern void register_edit_replace_command(void);
extern void register_wc_command(void);
extern void register_sort_command(void);
extern void register_date_command(void);
extern void register_find_command(void);
extern void register_edit_show_command(void);

static void register_all_commands(void) {
    register_ls_command();
    register_cat_command();
    register_stat_command();
    register_head_command();
    register_tail_command();
    register_cp_command();
    register_mv_command();
    register_rm_command();
    register_mkdir_command();
    register_rmdir_command();
    register_touch_command();
    register_rg_command();
    register_ps_command();
    register_kill_command();
    register_wait_command();
    register_jobs_command();
    register_cd_command();
    register_pwd_command();
    register_echo_command();
    register_env_command();
    register_export_command();
    register_unset_command();
    register_type_command();
    register_edit_replace_line_command();
    register_edit_insert_line_command();
    register_edit_delete_line_command();
    register_edit_replace_command();
    register_wc_command();
    register_sort_command();
    register_date_command();
    register_find_command();
    register_edit_show_command();
}

/* =========================================================================
 * Portable basename helper
 * ===================================================================== */
static const char *cmd_basename(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* =========================================================================
 * preprocess — ensure ';', '<', '>' are standalone tokens
 *
 * Surrounds each special character with spaces so that the tokeniser
 * always sees them as separate tokens regardless of whether the user
 * typed spaces around them (e.g. "cmd1;cmd2" → "cmd1 ; cmd2").
 * Caller must free() the returned string.
 * ===================================================================== */
static char *preprocess(const char *line) {
    size_t n   = strlen(line);
    char  *out = malloc(n * 3 + 1);   /* worst case: every char becomes " X " */
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if (line[i] == ';' || line[i] == '<' || line[i] == '>') {
            out[j++] = ' ';
            out[j++] = line[i];
            out[j++] = ' ';
        } else {
            out[j++] = line[i];
        }
    }
    out[j] = '\0';
    return out;
}

/* =========================================================================
 * Phase 1 — tokenise  (mirrors Token.c)
 *
 * Split 'line' in-place into a NULL-terminated token[] array.
 * Returns the number of tokens found.
 * ===================================================================== */
static int tokenise(char *line, char *token[]) {
    char *saveptr;
    char *tk = strtok_r(line, TOKEN_SEPARATORS, &saveptr);
    int   i  = 0;
    while (tk != NULL && i < MAX_NUM_TOKENS) {
        token[i++] = tk;
        tk = strtok_r(NULL, TOKEN_SEPARATORS, &saveptr);
    }
    token[i] = NULL;
    return i;
}

/* =========================================================================
 * Phase 2a — search_redirection  (mirrors searchRedirection in Command.c)
 *
 * Scan token[first..last] for '<' and '>', set stdin_file / stdout_file.
 * Pointers remain into the token[] array (which lives in the line buffer).
 * ===================================================================== */
static void search_redirection(char *token[], cmd_t *cmd) {
    for (int i = cmd->first; i <= cmd->last; i++) {
        if (strcmp(token[i], "<") == 0) {
            if (i + 1 <= cmd->last) cmd->stdin_file = token[++i];
        } else if (strcmp(token[i], ">") == 0) {
            if (i + 1 <= cmd->last) cmd->stdout_file = token[++i];
        }
    }
}

/* =========================================================================
 * Phase 2b — build_cmd_argv  (mirrors buildCommandArgumentArray in Command.c)
 *
 * Walk token[first..last], skip '<'/'>' operators and their filenames,
 * and expand each remaining token through glob(GLOB_NOCHECK) so that
 * wildcards like '*.c' are resolved.  Each result is strdup'd into a
 * heap-allocated argv[].
 *
 * GLOB_NOCHECK: if no file matches a pattern, the pattern is passed
 * through unchanged (POSIX behaviour for shells).
 * ===================================================================== */
static void build_cmd_argv(char *token[], cmd_t *cmd) {
    glob_t gr;
    int    n = 0;

    /* Pass 1 — count total expanded args */
    for (int t = cmd->first; t <= cmd->last; t++) {
        if (strcmp(token[t], ">") == 0 || strcmp(token[t], "<") == 0) {
            t++;        /* skip operator + filename */
            continue;
        }
        glob(token[t], GLOB_NOCHECK, NULL, &gr);
        n += (int)gr.gl_pathc;
        globfree(&gr);
    }

    cmd->argv = malloc(((size_t)n + 1) * sizeof(char *));
    if (!cmd->argv) { perror("aishell: malloc"); exit(1); }

    /* Pass 2 — fill argv with strdup'd glob results */
    int k = 0;
    for (int t = cmd->first; t <= cmd->last; t++) {
        if (strcmp(token[t], ">") == 0 || strcmp(token[t], "<") == 0) {
            t++;
            continue;
        }
        glob(token[t], GLOB_NOCHECK, NULL, &gr);
        for (size_t j = 0; j < gr.gl_pathc; j++)
            cmd->argv[k++] = strdup(gr.gl_pathv[j]);
        globfree(&gr);
    }
    cmd->argv[k] = NULL;
    cmd->argc    = k;
}

/* =========================================================================
 * Phase 2 — separate_commands  (mirrors separateCommands in Command.c)
 *
 * Build a cmd_t[] from the flat token[].  Only ';' is recognised as a
 * separator (no '|' or '&' — those are out of scope for this shell).
 * Returns the number of commands parsed, or 0 on empty input.
 * ===================================================================== */
static int separate_commands(char *token[], cmd_t cmds[]) {
    int ntokens = 0;
    while (token[ntokens] != NULL) ntokens++;
    if (ntokens == 0) return 0;

    /* Append an implicit ';' if the last token is not already one */
    if (strcmp(token[ntokens - 1], ";") != 0)
        token[ntokens++] = ";";   /* literal — safe, no free needed */

    int first = 0, c = 0;
    for (int i = 0; i < ntokens; i++) {
        if (strcmp(token[i], ";") == 0) {
            if (first == i) { first = i + 1; continue; }  /* skip empty */
            cmds[c].first       = first;
            cmds[c].last        = i - 1;
            cmds[c].sep         = token[i];
            cmds[c].argv        = NULL;
            cmds[c].argc        = 0;
            cmds[c].stdin_file  = NULL;
            cmds[c].stdout_file = NULL;
            c++;
            first = i + 1;
        }
    }

    for (int i = 0; i < c; i++) {
        search_redirection(token, &cmds[i]);
        build_cmd_argv(token, &cmds[i]);
    }
    return c;
}

/* =========================================================================
 * free_cmd_argv — release heap memory allocated by build_cmd_argv
 * ===================================================================== */
static void free_cmd_argv(cmd_t *cmd) {
    if (cmd->argv) {
        for (int i = 0; cmd->argv[i]; i++) free(cmd->argv[i]);
        free(cmd->argv);
        cmd->argv = NULL;
    }
}

/* =========================================================================
 * Phase 3 — execute_command  (mirrors execute_command / Builtin.c)
 *
 * Handle shell built-ins (exit, prompt), set up I/O redirections,
 * then dispatch through the command registry.
 *
 * Returns 0 on success, 127 if command not found,
 * or -(exit_code + 2) as a sentinel to signal the REPL to exit.
 * ===================================================================== */
static int execute_command(cmd_t *cmd, char *prompt, size_t prompt_sz) {
    if (cmd->argc == 0) return 0;

    /* Shell built-in: exit [N] */
    if (strcmp(cmd->argv[0], "exit") == 0) {
        int code = (cmd->argc > 1) ? atoi(cmd->argv[1]) : 0;
        return -(code + 2);
    }

    /* Shell built-in: prompt STRING — reconfigure the prompt string */
    if (strcmp(cmd->argv[0], "prompt") == 0) {
        if (cmd->argc > 1)
            snprintf(prompt, prompt_sz, "%s ", cmd->argv[1]);
        return 0;
    }

    /* ---- I/O redirections ---- */
    int saved_in = -1, saved_out = -1, ret = 0;

    if (cmd->stdin_file) {
        int fd = open(cmd->stdin_file, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "aishell: %s: %s\n", cmd->stdin_file, strerror(errno));
            return 1;
        }
        saved_in = dup(STDIN_FILENO);
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
    if (cmd->stdout_file) {
        int fd = open(cmd->stdout_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            fprintf(stderr, "aishell: %s: %s\n", cmd->stdout_file, strerror(errno));
            if (saved_in >= 0) { dup2(saved_in, STDIN_FILENO); close(saved_in); }
            return 1;
        }
        saved_out = dup(STDOUT_FILENO);
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }

    /* ---- Registry dispatch (strip path prefix: "./ls" → "ls") ---- */
    const char *cmdname = cmd_basename(cmd->argv[0]);
    const cmd_spec_t *spec = find_command(cmdname);
    if (spec) {
        ret = spec->run(cmd->argc, cmd->argv);
    } else {
        fprintf(stderr, "aishell: %s: command not found\n", cmd->argv[0]);
        ret = 127;
    }

    /* ---- Restore redirections ---- */
    if (saved_in  >= 0) { dup2(saved_in,  STDIN_FILENO);  close(saved_in);  }
    if (saved_out >= 0) { dup2(saved_out, STDOUT_FILENO); close(saved_out); }

    return ret;
}

/* =========================================================================
 * Interactive REPL  (mirrors main() in Unix_Shell.c)
 *
 *   Initialize  — signal mask, prompt
 *   Interpret   — getline with EINTR retry, tokenise, separate_commands
 *   Execute     — execute_command for each cmd_t
 *   Terminate   — exit sentinel propagated from execute_command
 * ===================================================================== */
static void shell_repl(void) {
    /* Block SIGINT, SIGQUIT, SIGTSTP — shell cannot be killed by Ctrl-C/Z/\ */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGQUIT);
    sigaddset(&mask, SIGTSTP);
    sigprocmask(SIG_SETMASK, &mask, NULL);

    char    prompt[256] = "jshell% ";
    char   *line = NULL;
    size_t  len  = 0;
    ssize_t nread;

    for (;;) {
        /* Colored prompt (yellow) on a real terminal, plain otherwise */
        if (isatty(STDOUT_FILENO))
            printf("\033[0;33m%s\033[0m", prompt);
        else
            fputs(prompt, stdout);
        fflush(stdout);

    retry:
        errno = 0;
        nread = getline(&line, &len, stdin);
        if (nread < 0) {
            if (errno == EINTR) { errno = 0; goto retry; }  /* slow syscall */
            putchar('\n');
            break;  /* EOF (Ctrl-D) */
        }

        /* Preprocess: pad ';' '<' '>' with spaces so they tokenise cleanly */
        char *processed = preprocess(line);
        if (!processed) { perror("aishell: malloc"); continue; }

        /* token[] needs MAX_NUM_TOKENS slots + 1 for implicit ';' + 1 NULL */
        char **token = calloc((size_t)(MAX_NUM_TOKENS + 2), sizeof(char *));
        if (!token) { perror("aishell: calloc"); free(processed); continue; }

        tokenise(processed, token);               /* Phase 1 */

        cmd_t cmds[MAX_NUM_COMMANDS];
        int ncmds = separate_commands(token, cmds); /* Phase 2 */

        for (int i = 0; i < ncmds; i++) {
            int r = execute_command(&cmds[i], prompt, sizeof(prompt)); /* Phase 3 */
            free_cmd_argv(&cmds[i]);

            if (r < -1) {           /* exit sentinel */
                free(token);
                free(processed);
                free(line);
                exit(-(r + 2));
            }
        }

        free(token);
        free(processed);   /* safe: cmd->stdin/stdout_file used before this */
    }

    free(line);
}

/* =========================================================================
 * main — three dispatch modes
 *
 *  1. BusyBox basename : invoked as a symlink/copy named after a command
 *                        e.g. ln -s aishell ls && ./ls /tmp
 *
 *  2. Subcommand       : ./aishell <cmd> [args...]
 *                        e.g. ./aishell ls /tmp
 *                             ./aishell stat test.txt
 *
 *  3. Interactive REPL : ./aishell  (no arguments)
 * ===================================================================== */
int main(int argc, char **argv) {
    register_all_commands();

    const char *base = cmd_basename(argv[0]);

    /* Mode 1 — BusyBox basename dispatch */
    if (strcmp(base, "aishell") != 0) {
        const cmd_spec_t *cmd = find_command(base);
        if (cmd) return cmd->run(argc, argv);
        fprintf(stderr, "aishell: %s: command not found\n", base);
        return 127;
    }

    /* Mode 2 — subcommand dispatch: ./aishell <cmd> [args...] */
    if (argc > 1) {
        const cmd_spec_t *cmd = find_command(argv[1]);
        if (cmd) {
            /* Shift: argv[1] becomes argv[0] so the command sees its own name */
            return cmd->run(argc - 1, argv + 1);
        }
        fprintf(stderr, "aishell: %s: command not found\n", argv[1]);
        return 127;
    }

    /* Mode 3 — interactive REPL */
    shell_repl();
    return 0;
}
