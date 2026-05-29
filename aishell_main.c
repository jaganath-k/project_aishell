#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <glob.h>
#include <stdint.h>
#include <pthread.h>
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

/* JOB_TYPE_NONE == 0 doubles as the free-slot sentinel (zero-initialised by
 * the static declaration of job_table below). */
typedef enum { JOB_TYPE_NONE = 0, JOB_TYPE_PROCESS, JOB_TYPE_THREAD } job_type_t;

typedef struct {
    int          id;             /* job number shown to the user: [1], [2], … */
    job_type_t   job_type;       /* PROCESS (fork) or THREAD (pthread) or NONE */
    pid_t        pid;            /* set for JOB_TYPE_PROCESS */
    pthread_t    tid;            /* set for JOB_TYPE_THREAD  */
    char         command[256];   /* command string for display */
    job_status_t status;
    int          exit_code;
} job_t;

static job_t job_table[MAX_JOBS];
static int   job_count = 0;     /* next job id to assign */

/* Protects all reads and writes of job_table[] and job_count.
 * Must NOT be held while calling blocking syscalls (waitpid, read, write).
 * jobs_mark_done() is the one exception — it must be called with the lock
 * already held by the caller (the sigchld_reaper_thread). */
static pthread_mutex_t job_table_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Self-pipe used to safely signal the reaper thread from the SIGCHLD handler.
 * Signal handlers may only call async-signal-safe functions; pthread_mutex_lock
 * is NOT async-signal-safe, so the handler writes one byte here instead of
 * touching the job table directly.  The reaper thread blocks on read() and
 * does the actual waitpid + job table update with the mutex held. */
static int sigchld_pipe[2] = {-1, -1};

/* =========================================================================
 * cwd_mutex + g_cwd — mutex-protected current working directory snapshot.
 *
 * WHY a separate buffer instead of calling getcwd() directly in the prompt?
 *   getcwd() is a syscall that can return different results across calls if
 *   another thread calls chdir() between them.  Storing the result in g_cwd
 *   immediately after each successful chdir() and reading it under the same
 *   mutex gives the prompt a consistent, atomic snapshot.
 *
 * WHY is the thread approach still safe for foreground cd?
 *   lsh_execute() calls pthread_join() on the cd thread before returning to
 *   the REPL loop.  The join guarantees the cd thread has already returned —
 *   and therefore chdir() has already completed — before the shell dispatches
 *   the next command.  No two commands ever overlap, so there is no real
 *   concurrency on the working directory for foreground cd.  The mutex on
 *   g_cwd is still correct practice: it makes the invariant explicit and
 *   keeps the code safe if the design ever changes.
 * ===================================================================== */
static pthread_mutex_t cwd_mutex = PTHREAD_MUTEX_INITIALIZER;
static char            g_cwd[4096] = "";

/* Serialises the dup2 → run → restore sequence for built-in pipeline stages.
 * Declared here (not near run_pipeline) so lsh_exit() can destroy it. */
static pthread_mutex_t pipeline_io_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Add a background process entry; returns the job id (1-based). */
static int jobs_add(pid_t pid, const char *cmdstr) {
    pthread_mutex_lock(&job_table_mutex);
    int result = -1;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (job_table[i].job_type == JOB_TYPE_NONE) {
            job_table[i].id        = ++job_count;
            job_table[i].job_type  = JOB_TYPE_PROCESS;
            job_table[i].pid       = pid;
            job_table[i].tid       = 0;
            job_table[i].status    = JOB_RUNNING;
            job_table[i].exit_code = 0;
            snprintf(job_table[i].command, sizeof(job_table[i].command),
                     "%s", cmdstr);
            result = job_table[i].id;
            break;
        }
    }
    pthread_mutex_unlock(&job_table_mutex);
    if (result < 0) fprintf(stderr, "aishell: job table full\n");
    return result;
}

/* Reserve a thread job slot BEFORE pthread_create so bta->job_id is valid
 * as soon as the thread starts.  The real tid is filled in afterwards by
 * jobs_set_thread_tid().  Returns the job id (1-based), or -1 if full. */
static int jobs_reserve_thread_slot(const char *cmdstr) {
    pthread_mutex_lock(&job_table_mutex);
    int result = -1;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (job_table[i].job_type == JOB_TYPE_NONE) {
            job_table[i].id        = ++job_count;
            job_table[i].job_type  = JOB_TYPE_THREAD;
            job_table[i].pid       = 0;
            job_table[i].tid       = 0;   /* filled by jobs_set_thread_tid */
            job_table[i].status    = JOB_RUNNING;
            job_table[i].exit_code = 0;
            snprintf(job_table[i].command, sizeof(job_table[i].command),
                     "%s", cmdstr);
            result = job_table[i].id;
            break;
        }
    }
    pthread_mutex_unlock(&job_table_mutex);
    if (result < 0) fprintf(stderr, "aishell: job table full\n");
    return result;
}

/* Store the real pthread_t after a successful pthread_create. */
static void jobs_set_thread_tid(int job_id, pthread_t tid) {
    pthread_mutex_lock(&job_table_mutex);
    for (int i = 0; i < MAX_JOBS; i++) {
        if (job_table[i].job_type == JOB_TYPE_THREAD &&
            job_table[i].id == job_id) {
            job_table[i].tid = tid;
            break;
        }
    }
    pthread_mutex_unlock(&job_table_mutex);
}

/* Mark a process job done — CALLER must hold job_table_mutex.
 * Called only from sigchld_reaper_thread. */
static void jobs_mark_done(pid_t pid, int exit_code) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (job_table[i].job_type == JOB_TYPE_PROCESS &&
            job_table[i].pid == pid) {
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
    /* Collect tids of Done thread jobs so we can join them outside the lock.
     * pthread_join() blocks; holding job_table_mutex across it would prevent
     * bg_builtin_cleanup() (which also locks job_table_mutex) from running in
     * a still-running thread — deadlock.  We snapshot and clear the slot under
     * the lock, then join after releasing it. */
    pthread_t done_tids[MAX_JOBS];
    int       ndone = 0;

    pthread_mutex_lock(&job_table_mutex);
    for (int i = 0; i < MAX_JOBS; i++) {
        if (job_table[i].job_type == JOB_TYPE_NONE) continue;

        /* Process jobs: poll waitpid to catch exits the SIGCHLD handler missed */
        if (job_table[i].job_type == JOB_TYPE_PROCESS &&
            job_table[i].status   == JOB_RUNNING) {
            int   wstatus;
            pid_t r = waitpid(job_table[i].pid, &wstatus, WNOHANG);
            if (r > 0) {
                job_table[i].status    = JOB_DONE;
                job_table[i].exit_code = WIFEXITED(wstatus)
                                         ? WEXITSTATUS(wstatus)
                                         : 128 + WTERMSIG(wstatus);
            } else if (r < 0 && errno == ECHILD) {
                job_table[i].status = JOB_DONE;
            }
        }
        /* Thread jobs: status is updated by bg_builtin_cleanup when they exit */

        char label[16];
        if (job_table[i].status == JOB_RUNNING) {
            snprintf(label, sizeof(label), "Running");
        } else if (job_table[i].status == JOB_STOPPED) {
            snprintf(label, sizeof(label), "Stopped");
        } else {
            snprintf(label, sizeof(label), "Done(%d)", job_table[i].exit_code);
        }

        const char *type_str = (job_table[i].job_type == JOB_TYPE_THREAD)
                                ? "Thread" : "Process";
        printf("[%d]  %-10s %-8s %s &\n",
               job_table[i].id, label, type_str, job_table[i].command);

        /* Free the slot once the job is confirmed done.
         * For joinable thread jobs, stash the tid for joining after unlock. */
        if (job_table[i].status == JOB_DONE) {
            if (job_table[i].job_type == JOB_TYPE_THREAD &&
                job_table[i].tid != 0)
                done_tids[ndone++] = job_table[i].tid;
            job_table[i].job_type = JOB_TYPE_NONE;
        }
    }
    pthread_mutex_unlock(&job_table_mutex);

    /* Join outside the lock: bg_builtin_cleanup has already run (that's what
     * set status to JOB_DONE), so these joins return almost immediately. */
    for (int i = 0; i < ndone; i++)
        pthread_join(done_tids[i], NULL);
}

/* =========================================================================
 * SIGCHLD handler — safe self-pipe notification.
 *
 * pthread_mutex_lock is NOT async-signal-safe (POSIX.1-2017 §2.4.3).
 * Calling it here would risk deadlock: if the handler fires while a thread
 * holds job_table_mutex, the handler's lock attempt blocks forever.
 *
 * Solution — write one byte to a non-blocking pipe.  The write() syscall
 * IS async-signal-safe.  sigchld_reaper_thread reads from the other end
 * and does the waitpid + job table update with the mutex held safely.
 * ===================================================================== */
static void sigchld_handler(int sig) {
    (void)sig;
    char byte = 1;
    /* O_NONBLOCK prevents blocking if the pipe buffer is full (unlikely but
     * possible if many children exit before the reaper thread drains them). */
    write(sigchld_pipe[1], &byte, 1);
}

/* =========================================================================
 * sigchld_reaper_thread — processes SIGCHLD notifications from the pipe.
 *
 * Blocks on read() until the signal handler writes a byte, then drains all
 * finished children via waitpid(-1, WNOHANG) in a loop.  Acquiring the
 * mutex here is safe because this is a normal thread context, not a signal
 * handler — pthread_mutex_lock is allowed.
 * ===================================================================== */
static void *sigchld_reaper_thread(void *arg) {
    (void)arg;

    /* Block every signal in this thread.  Signals are process-wide but
     * delivered to one thread.  Blocking here ensures SIGCHLD always goes
     * to the main thread (which has the sigaction handler installed) and
     * never interrupts this thread mid-mutex-lock. */
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL);

    char byte;
    while (read(sigchld_pipe[0], &byte, 1) == 1) {
        int   wstatus;
        pid_t pid;
        pthread_mutex_lock(&job_table_mutex);
        while ((pid = waitpid(-1, &wstatus, WNOHANG)) > 0) {
            int code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus)
                                          : 128 + WTERMSIG(wstatus);
            jobs_mark_done(pid, code);   /* called with mutex held */
        }
        pthread_mutex_unlock(&job_table_mutex);
    }
    return NULL;
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

    /* Update g_cwd immediately after the successful chdir.
     * pthread_join() in lsh_execute() guarantees no other command is
     * running concurrently, so this write races with nothing in practice.
     * The mutex makes that guarantee explicit and keeps the read in the
     * prompt display safe regardless of future scheduling changes. */
    pthread_mutex_lock(&cwd_mutex);
    if (!getcwd(g_cwd, sizeof(g_cwd)))
        g_cwd[0] = '\0';   /* fallback: show empty rather than stale path */
    pthread_mutex_unlock(&cwd_mutex);

    return 0;
}

static int lsh_exit(cmd_t *cmd) {
    int code = (cmd->argc > 1) ? atoi(cmd->argv[1]) : 0;

    /* ---- Step 1: cancel and join all background thread jobs ----
     *
     * We must NOT hold job_table_mutex across pthread_join():
     *   bg_builtin_cleanup() — the cleanup handler registered inside every
     *   background thread — calls pthread_mutex_lock(&job_table_mutex).
     *   If we hold the mutex here while blocking in pthread_join(), the
     *   cleanup handler will deadlock waiting for the same mutex.
     *
     * Safe pattern: lock → snapshot tids → unlock → cancel all → join all.
     *   All cancels are issued before any join so every thread gets the
     *   cancel request as early as possible; we then drain them in order.
     */
    pthread_t tids[MAX_JOBS];
    int       ntids = 0;

    pthread_mutex_lock(&job_table_mutex);
    for (int i = 0; i < MAX_JOBS; i++) {
        /* Collect ALL thread jobs, not just JOB_RUNNING ones.
         * A Done thread that was never joined (e.g. the user never ran 'jobs')
         * still has a valid, joinable tid.  We cancel it (no-op if already
         * finished) then join it to free the thread stack. */
        if (job_table[i].job_type == JOB_TYPE_THREAD &&
            job_table[i].tid      != 0) {
            tids[ntids++] = job_table[i].tid;
        }
    }
    pthread_mutex_unlock(&job_table_mutex);

    /* Send cancel requests first (non-blocking) so threads can start winding
     * down concurrently, then join each one in turn. */
    for (int i = 0; i < ntids; i++)
        pthread_cancel(tids[i]);
    for (int i = 0; i < ntids; i++)
        pthread_join(tids[i], NULL);   /* blocks until cleanup handlers finish */

    /* ---- Step 2: reap any remaining background child processes ----
     *
     * The SIGCHLD reaper thread may have already handled most of these, but
     * it could be slightly behind if children exited just before we cancelled
     * our own threads.  A WNOHANG loop is cheap and ensures no zombies linger.
     */
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;

    /* ---- Step 3: destroy mutexes ----
     *
     * Safe to destroy only after all threads that could acquire these mutexes
     * have been joined (step 1) or are the detached reaper thread — which
     * only touches job_table_mutex and will be killed by exit() momentarily.
     * POSIX allows destroy on a mutex with no waiters; calling it here makes
     * memory sanitisers and Valgrind happy.
     */
    pthread_mutex_destroy(&job_table_mutex);
    pthread_mutex_destroy(&cwd_mutex);
    pthread_mutex_destroy(&pipeline_io_mutex);

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
        /* Search job_table for this job id — hold mutex for the lookup */
        pthread_mutex_lock(&job_table_mutex);
        for (int i = 0; i < MAX_JOBS; i++) {
            if (job_table[i].pid != 0 && job_table[i].id == (int)jid) {
                pid = job_table[i].pid;
                break;
            }
        }
        pthread_mutex_unlock(&job_table_mutex);
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
 * pthread support for built-in commands
 *
 * thread_args_t is heap-allocated by lsh_execute(), passed through the
 * void* boundary into builtin_thread_entry(), and freed after pthread_join().
 * ===================================================================== */
typedef struct {
    cmd_t *cmd;      /* full command struct (argv, argc, redirections …) */
    int    result;   /* exit code written by the thread, read after join  */
} thread_args_t;

/* =========================================================================
 * Built-in dispatch table
 *
 * Parallel arrays: builtin_str[i] is the command name,
 * builtin_func[i] is the function to run inside a pthread.
 * lsh_execute() walks this table before falling through to lsh_launch().
 * Declared before builtin_thread_entry so the entry function can reference
 * both arrays without forward declarations.
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

/* Forward declarations needed by bg_builtin_cleanup (defined later) */
static cmd_t *cmd_dup(const cmd_t *src);
static void   cmd_free(cmd_t *cmd);

/* =========================================================================
 * Background built-in thread support
 *
 * bg_thread_args_t extends thread_args_t with the job_id.  The job table
 * slot is reserved (jobs_reserve_thread_slot) BEFORE pthread_create so
 * that job_id is valid the instant the thread starts — avoiding the race
 * where a very fast thread runs its cleanup before lsh_execute can write
 * the job_id into bta.
 *
 * bg_builtin_cleanup is registered with pthread_cleanup_push inside the
 * thread.  It runs when the thread returns normally OR is cancelled.
 * It acquires job_table_mutex, marks the slot Done, then frees bta —
 * so the main thread never needs to touch bta again after pthread_detach.
 * ===================================================================== */
typedef struct {
    thread_args_t base;     /* must be first so (thread_args_t *) casts work */
    int           job_id;   /* set before pthread_create; read by cleanup      */
    int           cmd_owned; /* 1 = base.cmd is heap-alloc'd via cmd_dup();
                                cleanup must call cmd_free(base.cmd)          */
} bg_thread_args_t;

static void bg_builtin_cleanup(void *arg) {
    bg_thread_args_t *bta = (bg_thread_args_t *)arg;
    if (bta->job_id > 0) {
        pthread_mutex_lock(&job_table_mutex);
        for (int i = 0; i < MAX_JOBS; i++) {
            if (job_table[i].job_type == JOB_TYPE_THREAD &&
                job_table[i].id == bta->job_id) {
                job_table[i].status    = JOB_DONE;
                job_table[i].exit_code = bta->base.result;
                break;
            }
        }
        pthread_mutex_unlock(&job_table_mutex);
    }
    if (bta->cmd_owned) cmd_free(bta->base.cmd);
    free(bta);
}

static void *bg_builtin_thread_fn(void *arg) {
    bg_thread_args_t *bta = (bg_thread_args_t *)arg;

    /* Enable deferred cancellation.  The cleanup handler registered below
     * (bg_builtin_cleanup) is invoked automatically on cancellation, so the
     * job table slot is always freed and bta is always freed — no leak. */
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE,   NULL);
    pthread_setcanceltype (PTHREAD_CANCEL_DEFERRED, NULL);

    /* Block SIGCHLD — must stay in the main thread where the handler lives */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);

    /* Declare result BEFORE pthread_cleanup_push: the push/pop macros expand
     * to a {…} block, so anything declared inside is out of scope after pop. */
    int result = 0;

    pthread_cleanup_push(bg_builtin_cleanup, bta);

    const char *name = cmd_basename(bta->base.cmd->argv[0]);
    for (int i = 0; builtin_str[i] != NULL; i++) {
        if (strcmp(name, builtin_str[i]) == 0) {
            bta->base.result = builtin_func[i](bta->base.cmd);
            result = bta->base.result;
            break;
        }
    }

    pthread_cleanup_pop(1);   /* 1 = execute cleanup now (frees bta) */
    return (void *)(intptr_t)result;
}

/* Thread entry point — declared after the table so it can reference both
 * builtin_str[] and builtin_func[] without needing forward declarations. */
static void *builtin_thread_entry(void *arg) {
    /* Enable deferred cancellation so lsh_exit() can cancel foreground
     * built-ins that are blocked in a syscall (e.g. sort reading a large
     * file).  DEFERRED means the cancel only fires at a defined cancellation
     * point (read, write, sleep, etc.) — never mid-instruction, so no
     * data structure is left half-written. */
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE,   NULL);
    pthread_setcanceltype (PTHREAD_CANCEL_DEFERRED, NULL);

    /* Block SIGCHLD in this thread so the signal is always delivered to the
     * main thread (where sigaction installed the handler).  If SIGCHLD fired
     * here it could interrupt a pthread_mutex_lock call inside the built-in,
     * and the signal handler cannot safely acquire the same mutex. */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);

    thread_args_t *ta   = (thread_args_t *)arg;
    const char    *name = cmd_basename(ta->cmd->argv[0]);

    for (int i = 0; builtin_str[i] != NULL; i++) {
        if (strcmp(name, builtin_str[i]) == 0) {
            ta->result = builtin_func[i](ta->cmd);
            return (void *)(intptr_t)ta->result;
        }
    }

    /* Should never reach here — caller already verified the name exists */
    ta->result = 1;
    return (void *)(intptr_t)1;
}

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

    /* Layer 1 — built-ins run in a dedicated pthread
     *
     * pthread_create + pthread_join makes the built-in feel synchronous to
     * the user (the prompt only reappears after the thread exits) while
     * keeping the main thread free to handle signals between the create and
     * join calls.  The heap-allocated thread_args_t is freed after join so
     * no state leaks across commands.
     */
    for (int i = 0; builtin_str[i] != NULL; i++) {
        if (strcmp(name, builtin_str[i]) == 0) {

            if (cmd->background) {
                /* ---- Background built-in: detach and return immediately ---- */

                /* cd: chdir() is process-wide; a detached thread would change the
                 * directory at an unpredictable time relative to the next command.
                 * exit: calling exit() from a detached thread skips the
                 * pthread_cleanup_push handler registered in bg_builtin_thread_fn,
                 * leaking the bta allocation and bypassing the ordered shutdown in
                 * lsh_exit().  Both must always run as foreground (joined) threads. */
                if (strcmp(name, "cd")   == 0 ||
                    strcmp(name, "exit") == 0) {
                    fprintf(stderr, "%s: cannot be run in background\n", name);
                    return 1;
                }

                /* Build the display command string before heap-alloc */
                char cmdstr[256] = "";
                for (int j = 0; j < cmd->argc; j++) {
                    if (j) strncat(cmdstr, " ", sizeof(cmdstr)-strlen(cmdstr)-1);
                    strncat(cmdstr, cmd->argv[j], sizeof(cmdstr)-strlen(cmdstr)-1);
                }

                bg_thread_args_t *bta = malloc(sizeof(bg_thread_args_t));
                if (!bta) { perror("aishell: malloc"); return 1; }
                bta->base.cmd = cmd_dup(cmd);
                if (!bta->base.cmd) { free(bta); return 1; }
                bta->base.result = 0;
                bta->cmd_owned   = 1;

                /* Reserve slot BEFORE pthread_create so job_id is valid the
                 * instant the thread starts — bg_builtin_cleanup uses it. */
                int jid = jobs_reserve_thread_slot(cmdstr);
                if (jid < 0) { free(bta); return 1; }
                bta->job_id = jid;

                pthread_t tid;
                int rc = pthread_create(&tid, NULL, bg_builtin_thread_fn, bta);
                if (rc != 0) {
                    fprintf(stderr, "aishell: pthread_create: %s\n", strerror(rc));
                    /* Mark the pre-reserved slot free again */
                    pthread_mutex_lock(&job_table_mutex);
                    for (int k = 0; k < MAX_JOBS; k++) {
                        if (job_table[k].id == jid)
                            job_table[k].job_type = JOB_TYPE_NONE;
                    }
                    pthread_mutex_unlock(&job_table_mutex);
                    free(bta);
                    return 1;
                }

                /* Fill in the real tid now that pthread_create succeeded.
                 * Do NOT detach the thread: lsh_exit() and jobs_print_and_prune()
                 * need a joinable tid to reclaim the thread's stack.  Calling
                 * pthread_join() on a detached thread is POSIX UB. */
                jobs_set_thread_tid(jid, tid);

                printf("[%d] (thread)\n", jid);
                fflush(stdout);
                return 0;

            } else {
                /* ---- Foreground built-in: join and wait ---- */
                thread_args_t *ta = malloc(sizeof(thread_args_t));
                if (!ta) { perror("aishell: malloc"); return 1; }
                ta->cmd    = cmd;
                ta->result = 0;

                pthread_t tid;
                int rc = pthread_create(&tid, NULL, builtin_thread_entry, ta);
                if (rc != 0) {
                    fprintf(stderr, "aishell: pthread_create: %s\n", strerror(rc));
                    free(ta);
                    return 1;
                }

                pthread_join(tid, NULL);
                int result = ta->result;
                free(ta);
                return result;
            }
        }
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
 * cmd_dup / cmd_free — deep copy a cmd_t for background threads.
 *
 * Background built-in threads (help &, jobs &, …) receive a raw pointer
 * into the REPL's stack-allocated cmds[] array.  The REPL calls
 * free_cmd_argv() and reuses the array as soon as execute_command()
 * returns — which can happen while the background thread is still reading
 * argv[0] to look up the command name.  The result is a use-after-free
 * and a segfault (SIGSEGV).
 *
 * cmd_dup() makes a fully independent heap copy: new cmd_t, new argv[],
 * strdup'd strings, strdup'd stdin/stdout filenames.  The background
 * thread owns this copy and cmd_free() releases it in bg_builtin_cleanup.
 * ===================================================================== */
static cmd_t *cmd_dup(const cmd_t *src) {
    cmd_t *dst = malloc(sizeof(cmd_t));
    if (!dst) return NULL;
    *dst = *src;          /* bitwise copy of all scalar fields */

    /* Deep-copy argv (build_cmd_argv strdup'd each element already, so we
     * must strdup again so the bg thread has its own independent copy). */
    if (src->argc > 0 && src->argv) {
        dst->argv = malloc(((size_t)src->argc + 1) * sizeof(char *));
        if (!dst->argv) { free(dst); return NULL; }
        int i;
        for (i = 0; i < src->argc; i++) {
            dst->argv[i] = strdup(src->argv[i]);
            if (!dst->argv[i]) {
                for (int j = 0; j < i; j++) free(dst->argv[j]);
                free(dst->argv); free(dst); return NULL;
            }
        }
        dst->argv[i] = NULL;
    } else {
        dst->argv = NULL;
        dst->argc = 0;
    }

    /* stdin_file / stdout_file point into the line buffer (freed by the REPL
     * loop); dup them so the bg thread never reads a freed pointer. */
    dst->stdin_file  = src->stdin_file  ? strdup(src->stdin_file)  : NULL;
    dst->stdout_file = src->stdout_file ? strdup(src->stdout_file) : NULL;
    return dst;
}

static void cmd_free(cmd_t *cmd) {
    if (!cmd) return;
    if (cmd->argv) {
        for (int i = 0; cmd->argv[i]; i++) free(cmd->argv[i]);
        free(cmd->argv);
    }
    free(cmd->stdin_file);
    free(cmd->stdout_file);
    free(cmd);
}

/* =========================================================================
 * Pipeline thread support
 *
 * When a built-in command appears as a pipeline stage, it runs as a
 * pthread instead of a forked child.  Both cases share the same pipe()
 * file descriptors for data transport; only the execution model differs.
 *
 * dup2() is process-wide: all threads share one fd table.  Two concurrent
 * threads doing dup2(different_fds, STDOUT_FILENO) race — one thread's
 * write could reach the other's pipe.  pipeline_io_mutex (declared near the
 * other globals above) serialises the entire dup2 → run → fflush → restore
 * sequence for built-in stages.  Forked children are unaffected (separate
 * fd tables) and never compete for this mutex.
 * ===================================================================== */

/* Arguments passed through the void* boundary to pipeline_builtin_thread_fn */
typedef struct {
    cmd_t *cmd;
    int    pipe_read;   /* fd to dup2 onto STDIN_FILENO  (-1 = not needed) */
    int    pipe_write;  /* fd to dup2 onto STDOUT_FILENO (-1 = not needed) */
    int    result;      /* exit code written by thread, read after join     */
} pipeline_stage_args_t;

/* Stage handle — tracks either a forked child (pid) or a built-in thread */
typedef enum  { STAGE_FORK, STAGE_THREAD } stage_type_t;
typedef struct {
    stage_type_t          type;
    union { pid_t pid; pthread_t tid; };
    pipeline_stage_args_t *args;   /* non-NULL for STAGE_THREAD; freed after join */
} stage_handle_t;

/* Returns 1 if name matches a command in the builtin_str[] dispatch table */
static int is_builtin_cmd(const char *name) {
    for (int i = 0; builtin_str[i] != NULL; i++)
        if (strcmp(name, builtin_str[i]) == 0) return 1;
    return 0;
}

/* Thread entry for a built-in command that is a stage in a pipeline.
 *
 * Holds pipeline_io_mutex for the entire dup2 → run → restore sequence so
 * no two built-in stages race on the shared fd table.  The mutex is released
 * only after fds are fully restored, so the next built-in stage that acquires
 * it sees the original fd state before doing its own dup2.
 *
 * External stages (forked children) never touch this mutex — they have
 * private fd tables and can run concurrently with no conflict.
 */
static void *pipeline_builtin_thread_fn(void *arg) {
    pipeline_stage_args_t *s = (pipeline_stage_args_t *)arg;

    /* Enable deferred cancellation.  Pipeline threads are always foreground
     * (run_pipeline joins them), but setting the state explicitly documents
     * intent and keeps behaviour consistent across all built-in thread types. */
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE,   NULL);
    pthread_setcanceltype (PTHREAD_CANCEL_DEFERRED, NULL);

    /* Block SIGCHLD so it stays in the main thread where the handler lives */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);

    /* Hold the IO mutex for the entire dup2 → run → restore sequence */
    pthread_mutex_lock(&pipeline_io_mutex);

    /* Wire this stage into the pipeline */
    int saved_in  = -1, saved_out = -1;
    if (s->pipe_read >= 0) {
        saved_in = dup(STDIN_FILENO);
        dup2(s->pipe_read,  STDIN_FILENO);
        close(s->pipe_read);
    }
    if (s->pipe_write >= 0) {
        saved_out = dup(STDOUT_FILENO);
        dup2(s->pipe_write, STDOUT_FILENO);
        close(s->pipe_write);
    }

    /* Run the built-in with redirected stdin/stdout */
    const char *name = cmd_basename(s->cmd->argv[0]);
    s->result = 1;
    for (int i = 0; builtin_str[i] != NULL; i++) {
        if (strcmp(name, builtin_str[i]) == 0) {
            s->result = builtin_func[i](s->cmd);
            break;
        }
    }

    /* Flush before restoring so buffered output reaches the pipe, not the
     * terminal that STDOUT_FILENO will point to after the restore */
    if (saved_out >= 0) fflush(stdout);

    /* Restore original stdin/stdout */
    if (saved_in  >= 0) { dup2(saved_in,  STDIN_FILENO);  close(saved_in);  clearerr(stdin); }
    if (saved_out >= 0) { dup2(saved_out, STDOUT_FILENO); close(saved_out); }

    pthread_mutex_unlock(&pipeline_io_mutex);
    return (void *)(intptr_t)s->result;
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

    stage_handle_t handles[MAX_PIPELINE];
    for (int i = 0; i < n; i++) {
        handles[i].type = STAGE_FORK;
        handles[i].pid  = -1;
        handles[i].args = NULL;
    }

    /* Track every dup'd fd created for thread stages.
     * These fds must be explicitly closed in every fork child's close loop.
     *
     * WHY: dup() is called before any fork().  Every fork child inherits ALL
     * open fds including the dup'd copies.  A fork child that READS from a pipe
     * (e.g. wc -l, sort) will deadlock if it also holds the WRITE end of the
     * same pipe open — it waits for EOF that will never arrive because it itself
     * prevents that EOF.  The fork child's standard close loop only covers
     * pipes[j][0/1]; it doesn't know about dup'd thread fds.  We close them
     * explicitly here.
     *
     * FD_CLOEXEC is also set as belt-and-suspenders for the execvp() path —
     * exec closes them automatically even if we forget the manual close.  But
     * for registry commands that call _exit() instead of exec, only the explicit
     * close saves us. */
    int thread_fds[MAX_PIPELINE * 2];
    int n_thread_fds = 0;

    /* Flush all stdio buffers before any fork so buffered parent output
     * does not leak into pipe content via the child's inherited buffer. */
    fflush(NULL);

    for (int i = 0; i < n; i++) {
        if (cmds[i].argc == 0) continue;

        const char *name   = cmd_basename(cmds[i].argv[0]);
        int         is_blt = is_builtin_cmd(name);

        /* Determine pipe fds for this stage:
         *   read  from pipes[i-1][0]  if not the first stage
         *   write to   pipes[i][1]    if not the last  stage */
        int rd = (i > 0)     ? pipes[i-1][0] : -1;
        int wr = (i < n - 1) ? pipes[i][1]   : -1;

        if (is_blt) {
            /* ---- Built-in stage: run as a pthread ----
             *
             * FD ownership: dup() the pipe ends so the thread owns its own
             * copies while the parent keeps the originals.  The thread closes
             * its copies after dup2(); the parent's close loop closes the
             * originals.  No sharing → no double-close → no fd-number reuse
             * hazard between the two closes. */
            int rd_t = (rd >= 0) ? dup(rd) : -1;
            int wr_t = (wr >= 0) ? dup(wr) : -1;
            if ((rd >= 0 && rd_t < 0) || (wr >= 0 && wr_t < 0)) {
                perror("aishell: dup");
                if (rd_t >= 0) close(rd_t);
                if (wr_t >= 0) close(wr_t);
                break;
            }

            /* Register dup'd fds so fork children can close them explicitly,
             * and set FD_CLOEXEC as belt-and-suspenders for the exec() path. */
            if (rd_t >= 0) { thread_fds[n_thread_fds++] = rd_t; fcntl(rd_t, F_SETFD, FD_CLOEXEC); }
            if (wr_t >= 0) { thread_fds[n_thread_fds++] = wr_t; fcntl(wr_t, F_SETFD, FD_CLOEXEC); }

            pipeline_stage_args_t *args = malloc(sizeof(pipeline_stage_args_t));
            if (!args) { perror("aishell: malloc"); close(rd_t); close(wr_t); break; }
            args->cmd        = &cmds[i];
            args->pipe_read  = rd_t;   /* thread-owned dup; thread closes after dup2 */
            args->pipe_write = wr_t;   /* thread-owned dup; thread closes after dup2 */
            args->result     = 1;

            pthread_t tid;
            int rc = pthread_create(&tid, NULL, pipeline_builtin_thread_fn, args);
            if (rc != 0) {
                fprintf(stderr, "aishell: pthread_create: %s\n", strerror(rc));
                free(args);
                break;
            }
            handles[i].type = STAGE_THREAD;
            handles[i].tid  = tid;
            handles[i].args = args;

        } else {
            /* ---- External or registry stage: fork ---- */
            pid_t pid = fork();
            if (pid < 0) {
                perror("aishell: fork");
                break;
            }

            if (pid == 0) {
                /* ---- Child ---- */
                struct sigaction sa_dfl;
                sa_dfl.sa_handler = SIG_DFL;
                sigemptyset(&sa_dfl.sa_mask);
                sa_dfl.sa_flags = 0;
                sigaction(SIGINT,  &sa_dfl, NULL);
                sigaction(SIGQUIT, &sa_dfl, NULL);

                /* Wire this stage into the pipeline */
                if (rd >= 0) dup2(rd, STDIN_FILENO);
                if (wr >= 0) dup2(wr, STDOUT_FILENO);

                /* Close every pipe fd — we've dup2'd what we need */
                for (int j = 0; j < n - 1; j++) {
                    close(pipes[j][0]);
                    close(pipes[j][1]);
                }
                /* Close dup'd fds created for thread stages.  These are NOT
                 * in pipes[][] so the loop above misses them.  Without this
                 * explicit close, a registry command (_exit path, no exec)
                 * that reads from a pipe would hold the write end open and
                 * deadlock waiting for EOF that it itself prevents. */
                for (int k = 0; k < n_thread_fds; k++)
                    close(thread_fds[k]);

                /* Explicit file redirections override the pipe */
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

            handles[i].type = STAGE_FORK;
            handles[i].pid  = pid;
        }
    }

    /* Parent closes all pipe fds so readers see EOF when writers exit.
     * Must happen after all stages are launched — if done stage-by-stage,
     * a forked child might not yet have dup2'd before we close the fd. */
    for (int i = 0; i < n - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    /* Wait for every stage and collect the last stage's exit status.
     * SIGCHLD stays blocked so the reaper thread doesn't steal a child's
     * exit status before our waitpid call reaches it. */
    int last_status = 0;
    for (int i = 0; i < n; i++) {
        int stage_status = 0;

        if (handles[i].type == STAGE_THREAD) {
            void *retval;
            pthread_join(handles[i].tid, &retval);
            stage_status = handles[i].args->result;
            free(handles[i].args);

        } else if (handles[i].pid > 0) {
            int wstatus;
            pid_t r;
            do { r = waitpid(handles[i].pid, &wstatus, 0); } while (r < 0 && errno == EINTR);
            if (r > 0)
                stage_status = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus)
                                                  : 128 + WTERMSIG(wstatus);
        }

        if (i == n - 1) last_status = stage_status;
    }

    /* Restore the signal mask now that all pipeline stages are done */
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

    /* Snapshot the initial working directory so the prompt is accurate
     * from the very first line, before any cd has been run. */
    pthread_mutex_lock(&cwd_mutex);
    if (!getcwd(g_cwd, sizeof(g_cwd))) g_cwd[0] = '\0';
    pthread_mutex_unlock(&cwd_mutex);

    for (;;) {
        /* Build a display prompt that includes the current directory.
         *
         * The mutex read here and the mutex write in lsh_cd() form a
         * consistent pair: the prompt always shows the directory that was
         * current when the previous command returned, never a half-written
         * intermediate value from a concurrent chdir() — which the cd-
         * background guard prevents anyway, but the mutex makes it explicit.
         *
         * Example: [/home/user/aishell/week3] jshell%
         */
        char display_prompt[4400];   /* sizeof(g_cwd) + sizeof(prompt) + brackets */
        pthread_mutex_lock(&cwd_mutex);
        if (g_cwd[0])
            snprintf(display_prompt, sizeof(display_prompt),
                     "[%s] %s", g_cwd, prompt);
        else
            snprintf(display_prompt, sizeof(display_prompt), "%s", prompt);
        pthread_mutex_unlock(&cwd_mutex);

        /* Colored prompt on a real terminal; plain to stderr otherwise so
         * that piped output is not polluted with prompt text. */
        if (isatty(STDOUT_FILENO))
            printf("\033[0;33m%s\033[0m", display_prompt);
        else
            fputs(display_prompt, stderr);
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

    /* Self-pipe for safe SIGCHLD → mutex-protected reaper.
     * The write end is O_NONBLOCK so the signal handler never blocks.
     * The reaper thread reads from the read end and updates job_table
     * with the mutex held — something the signal handler cannot safely do. */
    if (pipe(sigchld_pipe) < 0) { perror("aishell: pipe"); return 1; }
    fcntl(sigchld_pipe[1], F_SETFL, O_NONBLOCK);

    pthread_t reaper_tid;
    pthread_create(&reaper_tid, NULL, sigchld_reaper_thread, NULL);
    pthread_detach(reaper_tid);   /* runs for the life of the shell */

    /* SIGCHLD handler now only writes one byte to sigchld_pipe[1]. */
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
