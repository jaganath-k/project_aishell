# AiShell — Week 5 Work Log
## Process Management

**Project:** AiShell — BusyBox-style shell in C  
**Week:** 5  
**Primary file:** `week3/aishell_main.c`  
**Test suite:** `week3/test_week5.sh` — 25 checks, all passing  

---

## 1. Overview

Week 5 transforms AiShell from a shell that could only run its own 32 built-in registry commands into a fully capable interactive shell that can launch any external program on the system. The core additions are `fork`/`exec` for external command execution, multi-stage pipelines, I/O redirection, background jobs with `&`, a SIGCHLD handler for automatic zombie prevention, and Ctrl+C signal isolation so the shell survives signals that kill foreground children.

---

## 2. Goals for the Week

| # | Goal | Status |
|---|------|--------|
| 1 | `fork`/`exec` for external command dispatch | ✅ Done |
| 2 | Multi-stage pipelines (`cmd1 \| cmd2 \| cmd3`) | ✅ Done |
| 3 | Output redirection (`>` truncate, `>>` append) | ✅ Done |
| 4 | Input redirection (`<`) | ✅ Done |
| 5 | Background jobs with `&` — returns `[N] PID` | ✅ Done |
| 6 | Job table with `jobs` built-in tracking state | ✅ Done |
| 7 | SIGCHLD handler — automatic zombie reaping | ✅ Done |
| 8 | Signal isolation — Ctrl+C kills child, not shell | ✅ Done |
| 9 | In-process I/O redirect for registry commands | ✅ Done |
| 10 | Glob wildcard expansion (`*.c`, `*.h`, `?`) | ✅ Done |
| 11 | Path-prefix strip (`./ls`, `/usr/bin/ls`, `ls`) | ✅ Done |

---

## 3. Features Implemented

### 3.1 fork/exec for External Commands

Previously, any command not found in the registry printed "command not found". Week 5 adds `lsh_launch()` — a fork/exec path that runs any executable on `PATH`.

**Execution flow:**
1. `fork()` creates a child process
2. Child restores SIGINT and SIGQUIT to `SIG_DFL` (shell had them set to `SIG_IGN`)
3. Child calls `execvp(argv[0], argv)` to replace itself with the target program
4. Parent waits with `waitpid()` (foreground) or records the PID in the job table (background)

**Path-prefix stripping:** The shell strips `./` and absolute path prefixes before looking up in the registry. This means `./ls`, `/usr/bin/ls`, and `ls` all resolve to the same registry command if it exists, or fall through to `execvp` if it does not.

---

### 3.2 Multi-Stage Pipelines

Pipelines (`cmd1 | cmd2 | cmd3`) are handled by `run_pipeline()`. The tokeniser sets `pipe_next = 1` on any command whose stdout feeds the next stage.

**Pipeline setup sequence:**
1. Block `SIGCHLD` during setup to prevent the reaper handler from racing with `waitpid`
2. Call `fflush(NULL)` before the first `fork` to prevent stdio buffer contamination — any buffered output in the parent must not be duplicated into child processes
3. Create `n-1` pipes for `n` stages using `pipe()`
4. For each stage: `fork` → restore signals → `dup2` pipe ends → close all pipe fds → `exec`
5. Parent closes all pipe ends, unblocks `SIGCHLD`, then `waitpid` loop collects all children
6. Returns the exit status of the last stage

**In-process built-in stages:** Registry commands that run inside the shell process (rather than forking) use a fd save/restore pattern under `pipeline_io_mutex`:
```
save stdout fd → dup2 pipe_write to stdout → run command → flush → restore stdout fd
```

---

### 3.3 I/O Redirection

The preprocessor pads `>`, `>>`, and `<` with spaces so they are always separated tokens. The `separate_commands()` parser extracts redirection targets into `cmd_t.stdout_file`, `cmd_t.stdout_append`, and `cmd_t.stdin_file`.

**Three redirection types:**

| Operator | Flag | Behaviour |
|----------|------|-----------|
| `>` | `O_WRONLY \| O_CREAT \| O_TRUNC` | Overwrite the file |
| `>>` | `O_WRONLY \| O_CREAT \| O_APPEND` | Append to the file |
| `<` | `O_RDONLY` | Read stdin from file |

**For external commands:** The child process opens the file after `fork` but before `exec`, then `dup2`s it to the appropriate standard fd.

**For registry commands (in-process):** The shell saves the current fd with `dup()`, opens the redirect target, `dup2`s it into place, runs the command, calls `fflush`, then restores the original fd.

---

### 3.4 Background Jobs (`&`)

When a command ends with `&`, `lsh_execute()` forks the child but does not wait — it records the child PID in the job table and returns immediately.

**Notification on creation:**
```
[1] 48271
```

**Job table entry:**
```c
typedef struct {
    int          id;
    pid_t        pid;
    char         command[256];
    job_status_t status;    /* JOB_RUNNING / JOB_DONE / JOB_STOPPED */
    int          exit_code;
} job_t;
```

**`jobs` built-in output:**
```
[1]  Running    sleep 60 &
[1]  Done(0)    sleep 0.1 &
```

Completed jobs are pruned from the table when `jobs` is next called. `waitpid(pid, WNOHANG)` polls each entry so the status is always accurate at print time.

---

### 3.5 SIGCHLD Handler — Zombie Prevention

Without a SIGCHLD handler, every finished background child becomes a zombie (a process in state `Z`) until the parent calls `waitpid`. The handler uses `waitpid(-1, WNOHANG)` in a loop to reap all available children atomically.

**Handler configuration:**
```c
struct sigaction sa;
sa.sa_handler = sigchld_handler;
sigemptyset(&sa.sa_mask);
sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
sigaction(SIGCHLD, &sa, NULL);
```

- `SA_RESTART` — interrupted slow syscalls (like `read` in `getline`) are automatically restarted rather than returning `EINTR`
- `SA_NOCLDSTOP` — the handler is not called when a child is stopped (e.g., `SIGTSTP`), only when it exits

The handler also updates the job table by calling `jobs_mark_done()` for each reaped PID, keeping the status visible to the `jobs` built-in.

---

### 3.6 Signal Isolation (Ctrl+C)

**Problem:** The shell and its foreground child share the same process group. Pressing Ctrl+C sends SIGINT to the entire group — killing both the child and the shell.

**Solution:** The shell sets SIGINT, SIGQUIT, and SIGTSTP to `SIG_IGN` at startup. Every `fork` child restores SIGINT and SIGQUIT to `SIG_DFL` before calling `exec`.

**Why `SIG_IGN` and not `sigprocmask`?**
`exec`'d programs can override `SIG_IGN` with `SIG_DFL` — the exec'd child gets normal signal behaviour. They cannot, however, unblock a signal that was blocked with `sigprocmask`. Using `SIG_IGN` at the shell level therefore correctly passes default behaviour through to children.

**Result:** Ctrl+C kills only the currently running foreground child. The shell prompt reappears on the next line.

---

### 3.7 Glob Wildcard Expansion

Arguments containing `*`, `?`, or `[` are expanded using `glob(3)` with `GLOB_NOCHECK`. `GLOB_NOCHECK` means if no files match, the original pattern is passed through unchanged — the same behaviour as bash.

Expansion happens in `build_cmd_argv()` after tokenisation, before the command is dispatched. Each glob result replaces the original token in the `argv[]` array.

---

## 4. Key Data Structures

### `cmd_t` — Command Descriptor

```c
typedef struct {
    int    first, last;    /* token range indices        */
    char  *sep;            /* separator (always ";")     */
    char **argv;           /* glob-expanded argument vector */
    int    argc;
    char  *stdin_file;     /* filename from < redirect   */
    char  *stdout_file;    /* filename from > or >> redirect */
    int    stdout_append;  /* 1 if >>, 0 if >            */
    int    background;     /* 1 if trailing &            */
    int    pipe_next;      /* 1 if stdout feeds next cmd */
} cmd_t;
```

### `job_t` — Background Job Entry

```c
typedef enum { JOB_RUNNING = 0, JOB_DONE, JOB_STOPPED } job_status_t;

typedef struct {
    int          id;
    pid_t        pid;
    char         command[256];
    job_status_t status;
    int          exit_code;
} job_t;

static job_t job_table[MAX_JOBS];
```

---

## 5. Tokeniser Pipeline (REPL)

```
getline (EINTR-safe loop)
  → preprocess()          pad ;  <  >  >>  &  | with spaces
  → tokenise()            flat token[] array
  → separate_commands()   cmd_t[] structs — fills argv, pipe_next,
                          stdin_file, stdout_file, background
  → execute_command()     dispatch per cmd_t
        ├── pipe_next set? → run_pipeline()
        └── single cmd    → lsh_execute()
              ├── built-in special (exit, help, prompt) → direct call
              ├── registry command → in_process with fd save/restore
              └── external → lsh_launch(): fork → SIG_DFL → dup2 → execvp
```

---

## 6. Test Suite — `test_week5.sh`

25 checks across 10 test sections:

| Section | What is tested | Checks |
|---------|---------------|--------|
| 1. fork/exec | External command runs via `/usr/bin/date` | 1 |
| 2. Built-in vs external dispatch | `echo`, `pwd`, `cd ; pwd` | 3 |
| 3. Background jobs | `sleep 0.1 &` returns `[1] PID` | 1 |
| 4. jobs built-in | `jobs` lists background process | 1 |
| 5. kill command | `kill` is a registered command | 1 |
| 6. Pipes | 2-stage, 3-stage, rg in pipeline, no stdio contamination | 5 |
| 7. I/O redirection | `>`, `>>`, `<`, registry command `>`, registry command `>>` | 5 |
| 8. Zombie prevention | No `Z` state processes after background job exits | 1 |
| 9. Sequential commands | `cmd1 ; cmd2`, multiple semicolons | 2 |
| 10. Edge cases | Empty input, unknown command, pipe + redirect combined, glob | 4 + 1 compiled |

**Final result: 25 / 25 passed**

---

## 7. Architecture Decisions

| Decision | Rationale |
|----------|-----------|
| `SIG_IGN` for SIGINT/SIGQUIT/SIGTSTP in shell | `exec`'d children can override `SIG_IGN` back to `SIG_DFL`; they cannot unblock a masked signal |
| `SA_RESTART` on SIGCHLD | Prevents `getline` from returning `EINTR` when a background job exits mid-read |
| `SA_NOCLDSTOP` on SIGCHLD | Avoids spurious handler calls when children are stopped by `SIGTSTP` |
| `fflush(NULL)` before first `fork` | Prevents buffered stdio output from being duplicated into all pipeline children |
| Block `SIGCHLD` during pipeline setup | Prevents the SIGCHLD handler from calling `waitpid` on a child that the pipeline setup code is about to call `waitpid` on — double-reap race |
| `GLOB_NOCHECK` | Unmatched patterns pass through unchanged, matching bash behaviour |
| `dup()`/`dup2()` save-restore for in-process redirects | Registry commands run inside the shell process; their fd changes must be undone before the next command |
| `waitpid(pid, WNOHANG)` polling in `jobs` | Status shown is always accurate at print time, not just at SIGCHLD delivery time |

---

## 8. Files Modified

| File | Change |
|------|--------|
| `aishell_main.c` | Added `lsh_launch`, `run_pipeline`, I/O redirect logic, job table, SIGCHLD handler, signal isolation, glob expansion, `preprocess` updates for `>>` and `\|` |
| `test_week5.sh` | New — 25-check process management test suite |
| `Makefile` | Already had `-Wall -Wextra -std=c11`; no change needed |

---

## 9. Build and Test

```sh
cd week3
make
bash test_week5.sh
```

Expected output:
```
=== aishell week5 test suite ===
  ...
  Passed: 25
  Failed: 0
```

---

*Work log prepared for Week 5 of the AiShell project.*
