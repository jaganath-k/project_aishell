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
#include <sys/wait.h>

#include "cmd_spec.h"

/* =========================================================================
 * Constants  (mirrors Token.h / Command.h from the reference)
 * ===================================================================== */
#define MAX_NUM_TOKENS    1000
#define MAX_NUM_COMMANDS  100
#define MAX_PIPELINE        16   /* max stages in a single pipeline */
#define TOKEN_SEPARATORS  " \t\r\n\a"

/* =========================================================================
 * Command structure  (mirrors Command.h)
 *
 *   first / last  — inclusive indices into the flat token[] array
 *   sep           — separator that follows the command (always ";")
 *   argv / argc   — glob-expanded argument vector (heap-allocated)
 *   stdin_file    — filename from '<' redirection (points into token[])
 *   stdout_file   — filename from '>' redirection (points into token[])
 *   background    — 1 if the command ends with '&', 0 for foreground
 *   pipe_next     — 1 if this command's stdout is piped into the next one
 * ===================================================================== */
typedef struct {
    int    first;
    int    last;
    char  *sep;
    char **argv;
    int    argc;
    char  *stdin_file;
    char  *stdout_file;
    int    stdout_append; /* 1 if >> was used (O_APPEND), 0 for > (O_TRUNC) */
    int    background;
    int    pipe_next;
} cmd_t;

/* =========================================================================
 * Job table — tracks background processes
 *
 * Each background fork adds one entry. SIGCHLD marks entries as done.
 * 'jobs' built-in prints the table; completed jobs are pruned there.
 * ===================================================================== */
#define MAX_JOBS 64

typedef enum { JOB_RUNNING = 0, JOB_DONE, JOB_STOPPED } job_status_t;

typedef struct {
    int          id;             /* job number shown to the user: [1], [2], … */
    pid_t        pid;            /* process ID of the background child */
    char         command[256];   /* command string for display */
    job_status_t status;
    int          exit_code;
} job_t;

static job_t job_table[MAX_JOBS];
static int   job_count = 0;     /* next job id to assign */

/* Add a new entry; returns the job id assigned (1-based). */
static int jobs_add(pid_t pid, const char *cmdstr) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (job_table[i].pid == 0) {
            job_table[i].id       = ++job_count;
            job_table[i].pid      = pid;
            job_table[i].status   = JOB_RUNNING;
            job_table[i].exit_code = 0;
            snprintf(job_table[i].command, sizeof(job_table[i].command),
                     "%s", cmdstr);
            return job_table[i].id;
        }
    }
    fprintf(stderr, "aishell: job table full\n");
    return -1;
}

/* Mark a job done (called from SIGCHLD handler). */
static void jobs_mark_done(pid_t pid, int exit_code) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (job_table[i].pid == pid) {
            job_table[i].status    = JOB_DONE;
            job_table[i].exit_code = exit_code;
            return;
        }
    }
}

/* =========================================================================
 * jobs_print_and_prune — active status check + formatted output.
 *
 * For every live slot in job_table:
 *   1. Call waitpid(pid, WNOHANG) to poll whether the child has exited.
 *      WNOHANG returns immediately: 0 = still running, >0 = just finished.
 *      This catches jobs that finished between SIGCHLD deliveries or before
 *      the handler ran, so the status shown is always accurate at print time.
 *   2. Print one line per job:
 *        [N]  Running    cmd &
 *        [N]  Done(0)    cmd &
 *        [N]  Stopped    cmd &
 *   3. Free the slot for any job whose status is Done.
 * ===================================================================== */
static void jobs_print_and_prune(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (job_table[i].pid == 0) continue;

        /* Active poll: did this child exit since we last checked? */
        if (job_table[i].status == JOB_RUNNING) {
            int   wstatus;
            pid_t r = waitpid(job_table[i].pid, &wstatus, WNOHANG);
            if (r > 0) {
                /* Child has exited — update the table entry */
                job_table[i].status    = JOB_DONE;
                job_table[i].exit_code = WIFEXITED(wstatus)
                                         ? WEXITSTATUS(wstatus)
                                         : 128 + WTERMSIG(wstatus);
            } else if (r < 0 && errno == ECHILD) {
                /* Already reaped by SIGCHLD handler — mark done */
                job_table[i].status = JOB_DONE;
            }
            /* r == 0 → still running, leave status as JOB_RUNNING */
        }

        /* Format status label */
        char label[16];
        if (job_table[i].status == JOB_RUNNING) {
            snprintf(label, sizeof(label), "Running");
        } else if (job_table[i].status == JOB_STOPPED) {
            snprintf(label, sizeof(label), "Stopped");
        } else {
            snprintf(label, sizeof(label), "Done(%d)", job_table[i].exit_code);
        }

        printf("[%d]  %-10s %s &\n",
               job_table[i].id,
               label,
               job_table[i].command);

        /* Free the slot once the job is confirmed done */
        if (job_table[i].status == JOB_DONE)
            job_table[i].pid = 0;
    }
}

/* =========================================================================
 * SIGCHLD handler — reap background children asynchronously.
 *
 * Using WNOHANG inside the handler avoids blocking the shell while still
 * preventing zombie processes. The loop drains all pending exits in one
 * signal delivery (multiple children can exit between two deliveries).
 * ===================================================================== */
static void sigchld_handler(int sig) {
    (void)sig;
    int   status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int code = WIFEXITED(status) ? WEXITSTATUS(status)
                                     : 128 + WTERMSIG(status);
        jobs_mark_done(pid, code);
    }
}

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
 * prepend_mysh_bin_to_path — add ~/.mysh/bin to the front of PATH once
 * on shell startup so that installed packages (including pkg itself) are
 * found by execvp without the user having to set anything manually.
 * ===================================================================== */
static void prepend_mysh_bin_to_path(void) {
    const char *home = getenv("HOME");
    if (!home) return;

    char mysh_bin[512];
    snprintf(mysh_bin, sizeof(mysh_bin), "%s/.mysh/bin", home);

    const char *old = getenv("PATH");
    char new_path[4096];
    if (old && old[0])
        snprintf(new_path, sizeof(new_path), "%s:%s", mysh_bin, old);
    else
        snprintf(new_path, sizeof(new_path), "%s", mysh_bin);

    setenv("PATH", new_path, 1 /* overwrite */);
}

/* =========================================================================
 * lsh_launch — fork + execvp; foreground or background based on cmd->background.
 *
 * Foreground (cmd->background == 0):
 *   Parent calls waitpid() and blocks until the child exits or is killed.
 *   EINTR is retried so a stray signal cannot orphan the child.
 *
 * Background (cmd->background == 1):
 *   Parent does NOT waitpid() — child runs concurrently.
 *   The child is recorded in job_table and its PID is printed:
 *     [1] 4823
 *   SIGCHLD (installed in main) reaps the child when it finishes.
 * ===================================================================== */
static int lsh_launch(cmd_t *cmd) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("aishell: fork");
        return 1;
    }

    if (pid == 0) {
        /* ---- Child process ---- */

        /* Restore default signal handlers inherited as SIG_IGN from the shell.
         * Without this the child is immune to Ctrl-C (SIGINT) and Ctrl-\ (SIGQUIT)
         * because the shell uses SIG_IGN so the prompt is not killed, but
         * exec'd programs expect SIG_DFL.  The reset must happen before execvp
         * because exec preserves dispositions that were SIG_IGN. */
        struct sigaction sa_dfl;
        sa_dfl.sa_handler = SIG_DFL;
        sigemptyset(&sa_dfl.sa_mask);
        sa_dfl.sa_flags = 0;
        sigaction(SIGINT,  &sa_dfl, NULL);
        sigaction(SIGQUIT, &sa_dfl, NULL);

        /* Input redirection: cmd < file */
        if (cmd->stdin_file) {
            int fd = open(cmd->stdin_file, O_RDONLY);
            if (fd < 0) { perror(cmd->stdin_file); _exit(1); }
            dup2(fd, STDIN_FILENO);
            close(fd);
        }

        /* Output redirection: cmd > file  (truncate)
         *                      cmd >> file (append)   */
        if (cmd->stdout_file) {
            int flags = O_WRONLY | O_CREAT |
                        (cmd->stdout_append ? O_APPEND : O_TRUNC);
            int fd = open(cmd->stdout_file, flags, 0644);
            if (fd < 0) { perror(cmd->stdout_file); _exit(1); }
            dup2(fd, STDOUT_FILENO);
            close(fd);
        }

        execvp(cmd->argv[0], cmd->argv);
        perror(cmd->argv[0]);
        _exit(127);
    }

    /* ---- Parent process ---- */
    if (cmd->background) {
        /* Background: record in job table, print [N] PID, return immediately */
        char cmdstr[256] = "";
        for (int i = 0; i < cmd->argc; i++) {
            if (i) strncat(cmdstr, " ", sizeof(cmdstr) - strlen(cmdstr) - 1);
            strncat(cmdstr, cmd->argv[i], sizeof(cmdstr) - strlen(cmdstr) - 1);
        }
        int jid = jobs_add(pid, cmdstr);
        printf("[%d] %d\n", jid, pid);
        fflush(stdout);
        return 0;
    }

    /* Foreground: block until child exits or is killed */
    int status;
    pid_t r;
    do {
        r = waitpid(pid, &status, WUNTRACED);
    } while (r < 0 && errno == EINTR);

    if (r < 0) {
        perror("aishell: waitpid");
        return 1;
    }

    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) {
        if (WTERMSIG(status) == SIGINT) putchar('\n');
        return 128 + WTERMSIG(status);
    }
    return 1;
}

/* =========================================================================
 * Built-in command implementations
 *
 * These MUST run in the parent shell process — never in a fork'd child:
 *   cd   — chdir() only changes the working directory of the calling process;
 *           running it in a child would leave the shell's cwd unchanged.
 *   exit — must terminate the shell itself, not a throwaway child.
 *   help — informational; kept here so it also runs without forking.
 *
 * Contrast with lsh_launch(): that forks a child and execs an external
 * program, so none of the above effects would reach the parent.
 * ===================================================================== */

static int lsh_cd(cmd_t *cmd) {
    const char *target;
    if (cmd->argc < 2) {
        target = getenv("HOME");
        if (!target) { fprintf(stderr, "cd: HOME not set\n"); return 1; }
    } else {
        target = cmd->argv[1];
    }
    if (chdir(target) != 0) { perror("cd"); return 1; }
    return 0;
}

static int lsh_exit(cmd_t *cmd) {
    int code = (cmd->argc > 1) ? atoi(cmd->argv[1]) : 0;
    exit(code);
}

static int lsh_help(cmd_t *cmd) {
    (void)cmd;
    printf("aishell — available built-in commands:\n");
    printf("  cd [DIR]     change working directory (default: HOME)\n");
    printf("  exit [N]     exit the shell with code N (default 0)\n");
    printf("  help         show this message\n");
    printf("  jobs               list background jobs\n");
    printf("  kill [-SIG] %%N    send signal to job N (default: SIGTERM)\n");
    printf("  kill [-SIG] PID   send signal to PID directly\n");
    printf("  prompt STR        change the prompt string for this session\n");
    printf("\nRegistered commands (32 total) — run <cmd> --help for details.\n");
    printf("External programs on PATH are also supported.\n");
    printf("Append & to any command to run it in the background.\n");
    return 0;
}

static int lsh_jobs(cmd_t *cmd) {
    (void)cmd;
    jobs_print_and_prune();
    return 0;
}

/* =========================================================================
 * lsh_kill — built-in kill: send SIGTERM to a job or a raw PID.
 *
 * Two forms:
 *   kill %N   — look up job number N in the job table, send SIGTERM to its PID
 *   kill PID  — send SIGTERM directly to the numeric PID
 *
 * An optional signal number may be specified first:
 *   kill -9 %1   — send SIGKILL to job 1
 *   kill -9 4823 — send SIGKILL to PID 4823
 *
 * Why this must be a built-in (not an external):
 *   The %N job-table syntax is shell-private state. An external /bin/kill
 *   has no access to the shell's job_table, so %N lookup must happen here.
 * ===================================================================== */
static int lsh_kill(cmd_t *cmd) {
    if (cmd->argc < 2) {
        fprintf(stderr, "kill: usage: kill [-SIG] %%job | PID\n");
        return 1;
    }

    /* Optional signal: kill -N ... or kill -SIGNAME ... */
    int   signum = SIGTERM;   /* default signal */
    int   argidx = 1;         /* index of the target argument */

    if (cmd->argv[1][0] == '-' && cmd->argc >= 3) {
        /* Try to parse as a numeric signal number */
        char *endptr;
        long  s = strtol(cmd->argv[1] + 1, &endptr, 10);
        if (*endptr == '\0' && s > 0 && s < 64) {
            signum = (int)s;
            argidx = 2;
        } else {
            fprintf(stderr, "kill: unknown signal: %s\n", cmd->argv[1]);
            return 1;
        }
    }

    const char *target = cmd->argv[argidx];
    pid_t pid = -1;

    if (target[0] == '%') {
        /* %N form: look up job number in the table */
        char  *endptr;
        long   jid = strtol(target + 1, &endptr, 10);
        if (*endptr != '\0' || jid <= 0) {
            fprintf(stderr, "kill: bad job spec: %s\n", target);
            return 1;
        }
        /* Search job_table for this job id */
        for (int i = 0; i < MAX_JOBS; i++) {
            if (job_table[i].pid != 0 && job_table[i].id == (int)jid) {
                pid = job_table[i].pid;
                break;
            }
        }
        if (pid == -1) {
            fprintf(stderr, "kill: %%%ld: no such job\n", jid);
            return 1;
        }
    } else {
        /* Raw PID form */
        char *endptr;
        long  p = strtol(target, &endptr, 10);
        if (*endptr != '\0' || p <= 0) {
            fprintf(stderr, "kill: invalid PID: %s\n", target);
            return 1;
        }
        pid = (pid_t)p;
    }

    if (kill(pid, signum) != 0) {
        perror("kill");
        return 1;
    }

    return 0;
}

/* =========================================================================
 * Built-in dispatch table
 *
 * Parallel arrays: builtin_str[i] is the command name,
 * builtin_func[i] is the function to call directly in the parent.
 * lsh_execute() walks this table before falling through to lsh_launch().
 * ===================================================================== */
static const char *builtin_str[] = {
    "cd",
    "exit",
    "help",
    "jobs",
    "kill",
    NULL        /* sentinel — keeps the loop termination simple */
};

static int (*builtin_func[])(cmd_t *) = {
    lsh_cd,
    lsh_exit,
    lsh_help,
    lsh_jobs,
    lsh_kill,
};

/* =========================================================================
 * lsh_execute — top-level command dispatcher
 *
 * Priority order:
 *   1. Built-in table (builtin_str[]) — runs in parent, no fork
 *   2. aishell registry (32 built-in commands) — runs in parent, no fork
 *   3. lsh_launch() — forks a child and execs the external program
 *
 * This layered dispatch means cd/exit/help are always handled correctly
 * in the parent, registered commands run in-process, and everything else
 * is launched as an external program.
 * ===================================================================== */
static int lsh_execute(cmd_t *cmd) {
    if (cmd->argc == 0) return 0;

    const char *name = cmd_basename(cmd->argv[0]);

    /* Layer 1 — explicit built-ins that must not fork */
    for (int i = 0; builtin_str[i] != NULL; i++) {
        if (strcmp(name, builtin_str[i]) == 0)
            return builtin_func[i](cmd);
    }

    /* Layer 2 — aishell registered commands (registry.c) */
    const cmd_spec_t *spec = find_command(name);
    if (spec)
        return spec->run(cmd->argc, cmd->argv);

    /* Layer 3 — external program: fork + execvp */
    return lsh_launch(cmd);
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
        /* '>>' must be checked before '>' so it becomes one token, not two. */
        if (line[i] == '>' && i + 1 < n && line[i + 1] == '>') {
            out[j++] = ' '; out[j++] = '>'; out[j++] = '>'; out[j++] = ' ';
            i++;            /* consume the second '>' */
        } else if (line[i] == ';' || line[i] == '<' || line[i] == '>'
                                  || line[i] == '&' || line[i] == '|') {
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
        } else if (strcmp(token[i], ">>") == 0) {
            /* Append — must be checked before single '>' */
            if (i + 1 <= cmd->last) {
                cmd->stdout_file   = token[++i];
                cmd->stdout_append = 1;
            }
        } else if (strcmp(token[i], ">") == 0) {
            if (i + 1 <= cmd->last) {
                cmd->stdout_file   = token[++i];
                cmd->stdout_append = 0;
            }
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

    cmd->background = 0;

    /* Pass 1 — count total expanded args (skip operators, &, and their operands) */
    for (int t = cmd->first; t <= cmd->last; t++) {
        if (strcmp(token[t], ">")  == 0 || strcmp(token[t], ">>") == 0
                                        || strcmp(token[t], "<")  == 0) {
            t++;        /* skip operator + filename */
            continue;
        }
        if (strcmp(token[t], "&") == 0) continue;   /* strip & from argv */
        glob(token[t], GLOB_NOCHECK, NULL, &gr);
        n += (int)gr.gl_pathc;
        globfree(&gr);
    }

    cmd->argv = malloc(((size_t)n + 1) * sizeof(char *));
    if (!cmd->argv) { perror("aishell: malloc"); exit(1); }

    /* Pass 2 — fill argv; detect & to set background flag */
    int k = 0;
    for (int t = cmd->first; t <= cmd->last; t++) {
        if (strcmp(token[t], ">")  == 0 || strcmp(token[t], ">>") == 0
                                        || strcmp(token[t], "<")  == 0) {
            t++;
            continue;
        }
        if (strcmp(token[t], "&") == 0) {
            cmd->background = 1;    /* & found: run this command in background */
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
 * Phase 2 — separate_commands
 *
 * Build a cmd_t[] from the flat token[].
 * Recognised separators:
 *   ';'  — sequential execution, pipe_next = 0
 *   '|'  — pipeline stage,       pipe_next = 1
 *
 * pipe_next = 1 on cmd[i] means cmd[i].stdout feeds cmd[i+1].stdin via pipe.
 * Returns the number of commands parsed, or 0 on empty input.
 * ===================================================================== */
static int separate_commands(char *token[], cmd_t cmds[]) {
    int ntokens = 0;
    while (token[ntokens] != NULL) ntokens++;
    if (ntokens == 0) return 0;

    /* Append an implicit ';' if the line doesn't already end with a separator */
    const char *last = token[ntokens - 1];
    if (strcmp(last, ";") != 0 && strcmp(last, "|") != 0)
        token[ntokens++] = ";";

    int first = 0, c = 0;
    for (int i = 0; i < ntokens; i++) {
        int is_semi = (strcmp(token[i], ";") == 0);
        int is_pipe = (strcmp(token[i], "|") == 0);
        if (!is_semi && !is_pipe) continue;
        if (first == i) { first = i + 1; continue; }   /* skip empty segment */

        cmds[c].first         = first;
        cmds[c].last          = i - 1;
        cmds[c].sep           = token[i];
        cmds[c].argv          = NULL;
        cmds[c].argc          = 0;
        cmds[c].stdin_file    = NULL;
        cmds[c].stdout_file   = NULL;
        cmds[c].stdout_append = 0;
        cmds[c].background    = 0;
        cmds[c].pipe_next     = is_pipe; /* 1 → connected to next stage */
        c++;
        first = i + 1;
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
 * run_pipeline — execute N commands connected by pipes.
 *
 * For a pipeline  cmd0 | cmd1 | ... | cmdN-1 :
 *   1. Create N-1 pipes.
 *   2. Fork one child per stage.
 *   3. Each child:
 *        - dup2(pipes[i-1][0], stdin)  if not first stage
 *        - dup2(pipes[i][1],   stdout) if not last  stage
 *        - close ALL pipe FDs          (after dup2, before exec)
 *        - apply any explicit < / > redirection (overrides the pipe)
 *        - exec the command
 *   4. Parent closes all pipe FDs immediately after the last fork,
 *      then waitpid for every child.
 *
 * Closing unused pipe ends in both parent and children is critical:
 *   - If the write end stays open in the parent, the reader (e.g. wc)
 *     never sees EOF and hangs forever.
 *   - If a child inherits extra write ends it never closes, the next
 *     stage in the pipeline stalls the same way.
 *
 * Built-in commands (registered in the registry) are run via their
 * run() function inside the forked child — so 'cd' in a pipeline won't
 * affect the parent shell, but that matches standard shell behaviour.
 * ===================================================================== */
static int run_pipeline(cmd_t *cmds, int n) {
    if (n > MAX_PIPELINE) {
        fprintf(stderr, "aishell: pipeline too long (max %d stages)\n", MAX_PIPELINE);
        return 1;
    }

    /* Block SIGCHLD for the duration of the pipeline setup and wait.
     * The background SIGCHLD handler uses waitpid(-1,...) which would
     * race with our per-PID waitpid calls below and steal the exit
     * status of pipeline children.  We unblock after all pipe FDs are
     * closed so the handler can run once all children are safely in
     * flight and the parent no longer holds write ends open. */
    sigset_t chld_mask, old_mask;
    sigemptyset(&chld_mask);
    sigaddset(&chld_mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &chld_mask, &old_mask);

    /* Create N-1 pipes */
    int pipes[MAX_PIPELINE - 1][2];
    for (int i = 0; i < n - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("aishell: pipe");
            for (int j = 0; j < i; j++) { close(pipes[j][0]); close(pipes[j][1]); }
            sigprocmask(SIG_SETMASK, &old_mask, NULL);
            return 1;
        }
    }

    pid_t pids[MAX_PIPELINE];
    for (int i = 0; i < n; i++) pids[i] = -1;

    /* Flush all stdio buffers before any fork.  Registry commands (ls, cat,
     * echo, pwd, …) write through stdio into the parent's glibc buffer.  If
     * the buffer is not empty when fork() is called, the child inherits a
     * copy of the unflushed bytes.  After dup2() redirects fd 1 to the pipe
     * write end, the child's subsequent fflush(NULL) pumps those stale bytes
     * into the pipe — so the previous command's output ends up mixed with the
     * current stage's output and confuses downstream readers (wc, sort, …). */
    fflush(NULL);

    for (int i = 0; i < n; i++) {
        if (cmds[i].argc == 0) { pids[i] = -1; continue; }

        pids[i] = fork();
        if (pids[i] < 0) {
            perror("aishell: fork");
            /* Don't close pipes here — the parent always-closes loop below
             * handles all of them.  Closing twice would be a double-close. */
            pids[i] = -1;
            break;
        }

        if (pids[i] == 0) {
            /* ---- Child i ---- */

            /* Step 0: restore signal defaults inherited as SIG_IGN from shell */
            struct sigaction sa_dfl;
            sa_dfl.sa_handler = SIG_DFL;
            sigemptyset(&sa_dfl.sa_mask);
            sa_dfl.sa_flags = 0;
            sigaction(SIGINT,  &sa_dfl, NULL);
            sigaction(SIGQUIT, &sa_dfl, NULL);

            /* Step 1: wire this stage into the pipeline */
            if (i > 0)     dup2(pipes[i-1][0], STDIN_FILENO);   /* read from prev */
            if (i < n - 1) dup2(pipes[i][1],   STDOUT_FILENO);  /* write to next  */

            /* Step 2: close every pipe FD — we've dup2'd what we need */
            for (int j = 0; j < n - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            /* Step 3: explicit file redirections override the pipe.
             * Supports both > (truncate) and >> (append). */
            if (cmds[i].stdin_file) {
                int fd = open(cmds[i].stdin_file, O_RDONLY);
                if (fd < 0) { perror(cmds[i].stdin_file); _exit(1); }
                dup2(fd, STDIN_FILENO); close(fd);
            }
            if (cmds[i].stdout_file) {
                int flags = O_WRONLY | O_CREAT |
                            (cmds[i].stdout_append ? O_APPEND : O_TRUNC);
                int fd = open(cmds[i].stdout_file, flags, 0644);
                if (fd < 0) { perror(cmds[i].stdout_file); _exit(1); }
                dup2(fd, STDOUT_FILENO); close(fd);
            }

            /* Step 4: exec — try registry first, then external.
             * fflush(NULL) before _exit: registered commands write to stdio
             * (printf/fprintf) which is fully buffered when stdout is a pipe.
             * _exit() skips stdio cleanup so buffered output would be lost.
             * Flushing explicitly ensures all output reaches the pipe reader. */
            const char *name = cmd_basename(cmds[i].argv[0]);
            const cmd_spec_t *spec = find_command(name);
            if (spec) {
                int rc = spec->run(cmds[i].argc, cmds[i].argv);
                fflush(NULL);
                _exit(rc);
            } else {
                execvp(cmds[i].argv[0], cmds[i].argv);
                perror(cmds[i].argv[0]);
                _exit(127);
            }
        }
    }

    /* Parent: close all pipe FDs so readers see EOF when writers exit */
    for (int i = 0; i < n - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    /* Wait for all children; return the exit status of the last stage.
     * SIGCHLD remains blocked until after this loop so the handler cannot
     * race with our per-PID waitpid calls and steal a child's exit status. */
    int last_status = 0;
    for (int i = 0; i < n; i++) {
        if (pids[i] <= 0) continue;
        int wstatus;
        pid_t r;
        do { r = waitpid(pids[i], &wstatus, 0); } while (r < 0 && errno == EINTR);
        if (i == n - 1 && r > 0)
            last_status = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus)
                                             : 128 + WTERMSIG(wstatus);
    }

    /* Restore the signal mask now that all pipeline children are reaped */
    sigprocmask(SIG_SETMASK, &old_mask, NULL);
    return last_status;
}

/* =========================================================================
 * Phase 3 — execute_command
 *
 * Sets up I/O redirections, then delegates all dispatch to lsh_execute():
 *   built-in table (cd/exit/help) → aishell registry → lsh_launch()
 *
 * 'prompt' is the only thing handled here directly because it modifies
 * the REPL's own state (the prompt string) rather than running a command.
 * ===================================================================== */
static int execute_command(cmd_t *cmd, char *prompt, size_t prompt_sz) {
    if (cmd->argc == 0) return 0;

    /* prompt is a REPL meta-command — changes the shell's own prompt string */
    if (strcmp(cmd->argv[0], "prompt") == 0) {
        if (cmd->argc > 1)
            snprintf(prompt, prompt_sz, "%s ", cmd->argv[1]);
        return 0;
    }

    /* ---- I/O redirections ----
     *
     * External commands (those that reach lsh_launch) set up their own
     * redirections in the forked child — the standard Unix approach.
     * Commands that run in the parent process (built-in table and the
     * aishell registry) cannot use that pattern, so we apply a
     * dup-redirect-run-restore sequence here instead.
     *
     * We detect "in-process" by checking the dispatch layers in order:
     * builtin_str[] first, then the registry.  Anything not found in
     * either will end up in lsh_launch(), which owns its own I/O setup.
     */
    const char *name = cmd_basename(cmd->argv[0]);
    int in_process = 0;
    for (int i = 0; builtin_str[i] != NULL; i++) {
        if (strcmp(name, builtin_str[i]) == 0) { in_process = 1; break; }
    }
    if (!in_process && find_command(name) != NULL) in_process = 1;

    int saved_in = -1, saved_out = -1, ret = 0;

    if (in_process) {
        /* Dup/restore: redirect in parent, restore after run */
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
            int flags = O_WRONLY | O_CREAT |
                        (cmd->stdout_append ? O_APPEND : O_TRUNC);
            int fd = open(cmd->stdout_file, flags, 0644);
            if (fd < 0) {
                fprintf(stderr, "aishell: %s: %s\n", cmd->stdout_file, strerror(errno));
                if (saved_in >= 0) { dup2(saved_in, STDIN_FILENO); close(saved_in); }
                return 1;
            }
            saved_out = dup(STDOUT_FILENO);
            dup2(fd, STDOUT_FILENO);
            close(fd);
        }
    }

    /* ---- Dispatch: built-in table → registry → external (lsh_execute) ---- */
    ret = lsh_execute(cmd);

    /* Flush stdio before restoring fds.  glibc's full-buffered mode does not
     * guarantee a flush on '\n'; if the buffer drains after dup2 restores
     * STDOUT_FILENO to the terminal, the command's output goes to the wrong
     * destination.  Flushing here commits all output to the redirected fd
     * before we hand it back. */
    if (saved_out >= 0) fflush(stdout);

    /* ---- Restore redirections (in-process commands only) ---- */
    if (saved_in  >= 0) {
        dup2(saved_in, STDIN_FILENO);
        close(saved_in);
        /* A registry command that read stdin to EOF (e.g. wc, sort) leaves
         * the feof() flag set on glibc's FILE *stdin.  dup2 restores the
         * underlying fd but does not clear the cached EOF state, so the
         * next getline() call would return -1 and exit the REPL.
         * clearerr() resets both the EOF and error flags. */
        clearerr(stdin);
    }
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
    /* Shell ignores SIGINT, SIGQUIT, and SIGTSTP so that Ctrl-C / Ctrl-\ /
     * Ctrl-Z don't kill or suspend the interactive shell itself.
     *
     * SIG_IGN rather than sigprocmask(SIG_BLOCK):
     *   A blocked signal is inherited by exec'd children still blocked —
     *   Ctrl-C would never reach them even with SIG_DFL.
     *   A SIG_IGN disposition IS inherited, but children can override it with
     *   sigaction(SIG_DFL) before exec (see lsh_launch / run_pipeline).
     *
     * SIGTSTP: left as SIG_IGN (no full job-control implemented yet).
     * SIGINT / SIGQUIT: children restore SIG_DFL so Ctrl-C / Ctrl-\ kill them.
     */
    struct sigaction sa_ign;
    sa_ign.sa_handler = SIG_IGN;
    sigemptyset(&sa_ign.sa_mask);
    sa_ign.sa_flags = 0;
    sigaction(SIGINT,  &sa_ign, NULL);
    sigaction(SIGQUIT, &sa_ign, NULL);
    sigaction(SIGTSTP, &sa_ign, NULL);

    char    prompt[256] = "jshell% ";
    char   *line = NULL;
    size_t  len  = 0;
    ssize_t nread;

    for (;;) {
        /* Colored prompt on a real terminal; plain to stderr otherwise so
         * that piped output is not polluted with prompt text. */
        if (isatty(STDOUT_FILENO))
            printf("\033[0;33m%s\033[0m", prompt);
        else
            fputs(prompt, stderr);
        fflush(stdout);
        fflush(stderr);

        errno = 0;
        nread = getline(&line, &len, stdin);
        if (nread < 0) {
            if (errno == EINTR) {
                /* A signal (e.g. a future SIGINT handler without SA_RESTART)
                 * interrupted getline.  The terminal has already echoed "^C"
                 * and moved to a new line, so restart the whole loop to print
                 * a fresh prompt rather than retrying on the same line. */
                clearerr(stdin);
                errno = 0;
                continue;
            }
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

        /* Phase 3 — execute, grouping piped stages into pipelines */
        int i = 0;
        while (i < ncmds) {
            /* Scan forward to find the end of this pipeline group */
            int j = i;
            while (j < ncmds - 1 && cmds[j].pipe_next) j++;
            /* cmds[i..j] form a pipeline of (j - i + 1) stages */
            int plen = j - i + 1;
            if (plen == 1)
                execute_command(&cmds[i], prompt, sizeof(prompt));
            else
                run_pipeline(cmds + i, plen);

            for (int k = i; k <= j; k++) free_cmd_argv(&cmds[k]);
            i = j + 1;
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
    prepend_mysh_bin_to_path();   /* make ~/.mysh/bin visible to execvp */
    register_all_commands();

    /* Reap background children automatically — prevents zombie processes.
     * SA_RESTART makes library calls retry after the signal instead of
     * returning EINTR, so background reaping is transparent to the REPL. */
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

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
