# AiShell — Week 6 Work Log
## POSIX Thread (pthread) Integration

**Project:** AiShell — BusyBox-style shell in C  
**Week:** 6  
**Primary file:** `week3/aishell_main.c` (~1,835 lines)  
**Test suite:** `week3/test_week6.sh` — 23 checks, all passing  

---

## 1. Overview

Week 6 introduces POSIX thread (`pthread`) integration into the AiShell REPL. Previously, all built-in commands (such as `help`, `jobs`, `cd`) ran directly in the shell's main process. This week, every built-in command is dispatched through a thread — either a **foreground thread** (joined immediately, blocking the REPL) or a **background thread** (left running while the shell continues).

The key design constraint is that the shell must remain stable under all combinations: background processes mixed with background threads, piped built-ins mixed with external commands, and clean shutdown when the user types `exit`.

---

## 2. Features Implemented

### 2.1 Built-in Commands Run in Threads

**What changed:** `lsh_execute()` no longer calls built-in functions directly. It always creates a `pthread`, passes the command through a `thread_args_t` struct, and either joins the thread immediately (foreground) or leaves it running (background with `&`).

**Foreground path:**
```c
thread_args_t *ta = malloc(sizeof(thread_args_t));
ta->cmd    = cmd;
ta->result = 0;
pthread_create(&tid, NULL, builtin_thread_entry, ta);
pthread_join(tid, &retval);
free(ta);
```

**Why threads for foreground commands?**  
Uniformity. Every command goes through the same dispatch path. Built-ins that do I/O (like `help` writing to stdout) automatically benefit from the `pipeline_io_mutex` serialisation used by the pipe infrastructure.

---

### 2.2 Background Built-ins (`help &`, `jobs &`)

**What changed:** When a built-in command is followed by `&`, `lsh_execute()` creates a detached-style thread (kept joinable for cleanup) and registers it in the job table immediately.

**Job table entry for threads:**
```
[2] (thread)          ← printed on creation
[2]  Done(0)  Thread  help &   ← shown by jobs
```

**Job type enum extended:**
```c
typedef enum { JOB_TYPE_NONE = 0, JOB_TYPE_PROCESS, JOB_TYPE_THREAD } job_type_t;
```

**Guard against unsafe builtins in background:** `cd` and `exit` are rejected if the user appends `&`:
```
jshell% cd &
cd: cannot be run in background
jshell% exit &
exit: cannot be run in background
```

`cd` is rejected because `chdir()` is process-wide — running it in a background thread would race with the foreground REPL reading the current directory. `exit` is rejected because it would terminate the shell without cleaning up other threads.

---

### 2.3 Job Table — Slot Pre-reservation Pattern

**Problem:** If the thread is very fast, it can call `bg_builtin_cleanup()` (which reads `bta->job_id`) before `lsh_execute()` has written the job ID into `bta`. This is a race condition.

**Solution:** Reserve the job table slot (and assign the job ID) **before** calling `pthread_create`. The `tid` field is filled in afterwards via `jobs_set_thread_tid()` once `pthread_create` succeeds.

```c
int jid = jobs_reserve_thread_slot(cmdstr);  // before create
bta->job_id = jid;
pthread_create(&tid, NULL, bg_builtin_thread_fn, bta);
jobs_set_thread_tid(jid, tid);               // after create
```

---

### 2.4 Thread Cancellation and Clean Shutdown (`lsh_exit`)

**What changed:** `lsh_exit()` performs an ordered shutdown sequence before calling `exit()`:

1. **Collect thread IDs** from the job table (under the mutex)
2. **Cancel** each background thread (`pthread_cancel`)
3. **Join** each thread (`pthread_join`) — outside the lock to avoid deadlock
4. **Reap** background child processes (`waitpid(-1, NULL, WNOHANG)` loop)
5. **Destroy** all three mutexes (`job_table_mutex`, `cwd_mutex`, `pipeline_io_mutex`)
6. Call `exit(code)`

```c
for (int i = 0; i < ntids; i++) pthread_cancel(tids[i]);
for (int i = 0; i < ntids; i++) pthread_join(tids[i], NULL);
while (waitpid(-1, NULL, WNOHANG) > 0) ;
pthread_mutex_destroy(&job_table_mutex);
pthread_mutex_destroy(&cwd_mutex);
pthread_mutex_destroy(&pipeline_io_mutex);
exit(code);
```

**Cancellation mode:** Every thread sets `PTHREAD_CANCEL_DEFERRED` so cancellation only fires at safe syscall points — never mid-instruction while holding a mutex.

**Cleanup handler:** Each background thread registers `bg_builtin_cleanup()` via `pthread_cleanup_push`. This handler runs on both normal exit and on cancellation, ensuring the job table slot is always marked Done and `bta` is always freed — no memory leak regardless of how the thread terminates.

---

### 2.5 Mutex-Protected Working Directory (`g_cwd`)

**What changed:** A global `char g_cwd[4096]` buffer stores the current working directory, updated by `lsh_cd()` immediately after every successful `chdir()`, protected by `cwd_mutex`.

**Why not call `getcwd()` directly in the prompt?**  
`getcwd()` is a syscall that could return a different path if another thread called `chdir()` between two calls. Caching the result in `g_cwd` under a mutex gives the prompt a consistent, atomic snapshot.

**Prompt now reflects the current directory:**
```
[/tmp] jshell%
[/usr/local] jshell%
```

**`lsh_cd()` update sequence:**
```c
if (chdir(target) != 0) { perror("cd"); return 1; }
pthread_mutex_lock(&cwd_mutex);
if (!getcwd(g_cwd, sizeof(g_cwd))) g_cwd[0] = '\0';
pthread_mutex_unlock(&cwd_mutex);
return 0;
```

**Why `cd` is safe as a foreground thread:**  
`lsh_execute()` calls `pthread_join()` on the cd thread before returning. The join guarantees `chdir()` has completed before the next command is dispatched — no real concurrency on the working directory for foreground `cd`.

---

### 2.6 Pipeline Support: Built-in ↔ External Mixed Pipes

**What changed:** `run_pipeline()` now handles three types of pipeline stages:

| Stage type | Mechanism |
|-----------|-----------|
| External command | `fork()` + `execvp()` |
| Registry built-in (foreground, in pipeline) | Thread with dup'd pipe fds under `pipeline_io_mutex` |
| Background built-in | Separate joinable thread in job table |

**Thread-to-Fork pipe (`help | grep exit`):**  
The built-in thread writes to the write end of the pipe. The `grep` child reads from the read end. The thread holds a dup'd fd (`wr_t = dup(wr)`) so it owns its own independent file descriptor.

**Fork-to-Thread pipe (`echo hello | jobs`):**  
The fork child writes to the pipe; the built-in thread reads from it. The pipe must be closed properly so neither side hangs waiting for EOF.

**Critical fix — fork child inheriting thread fds:**  
Fork children inherit all open file descriptors, including the dup'd fds held by thread stages. If a fork child holds the write end of a pipe that another process is reading from, the reader never sees EOF and the pipeline deadlocks.

**Solution:** Track all thread dup'd fds in `thread_fds[]`; every fork child explicitly closes them:
```c
int thread_fds[MAX_PIPELINE * 2]; int n_thread_fds = 0;
// dup for thread stage:
int wr_t = dup(wr);
thread_fds[n_thread_fds++] = wr_t;
fcntl(wr_t, F_SETFD, FD_CLOEXEC);  // belt-and-suspenders for exec paths
// in every fork child:
for (int k = 0; k < n_thread_fds; k++) close(thread_fds[k]);
```

Note: `FD_CLOEXEC` closes the fd on `exec()` but NOT on `_exit()`. Registry commands that call `_exit()` directly could still leak the fd without the explicit close above.

---

### 2.7 Segfault Fix — Use-After-Free on `cmd_t`

**Bug reported:** Running `/usr/bin/sleep 5 &` followed by `help &` caused `Segmentation fault (core dumped)`.

**Root cause (use-after-free race):**
```
lsh_execute (background path)
  bta->base.cmd = cmd        ← raw pointer into REPL's stack-local cmds[]
  pthread_create(bg_thread)  ← thread starts, can run immediately
  return 0                   ← lsh_execute returns to REPL loop

REPL: free_cmd_argv(&cmds[i])       ← frees cmd->argv[]

bg thread: cmd->argv[0]             ← SIGSEGV: argv[] already freed
```

**Fix — deep copy the `cmd_t` for background threads:**

Added two helpers:

**`cmd_dup()`** — creates a fully independent heap copy of a `cmd_t`:
- New `cmd_t` on the heap (via `malloc`)
- New `argv[]` array with `strdup`'d strings for each argument
- `strdup`'d copies of `stdin_file` and `stdout_file` (which point into the line buffer freed by the REPL)

**`cmd_free()`** — releases everything allocated by `cmd_dup()`.

**Wiring:**
```c
// bg_thread_args_t: new field
int cmd_owned;  /* 1 = free base.cmd in cleanup */

// lsh_execute background path:
bta->base.cmd = cmd_dup(cmd);   // independent copy
bta->cmd_owned = 1;

// bg_builtin_cleanup:
if (bta->cmd_owned) cmd_free(bta->base.cmd);
free(bta);
```

After the fix: the background thread owns its private copy of `cmd_t` and all argument strings. The REPL is free to call `free_cmd_argv()` at any time without affecting the thread.

---

## 3. Global State Added (Thread-Safe)

| Variable | Type | Purpose |
|----------|------|---------|
| `job_table_mutex` | `pthread_mutex_t` | Serialises all reads/writes of `job_table[]` and `job_count` |
| `sigchld_pipe[2]` | `int[2]` | Self-pipe: SIGCHLD handler writes one byte; reaper thread reads and calls `waitpid` |
| `cwd_mutex` | `pthread_mutex_t` | Protects `g_cwd[]` during `chdir` and prompt rendering |
| `g_cwd[4096]` | `char[]` | Cached current working directory, updated after each `cd` |
| `pipeline_io_mutex` | `pthread_mutex_t` | Serialises dup2 → run → restore for built-in pipeline stages |

---

## 4. Bugs Found and Fixed

| # | Bug | Root Cause | Fix |
|---|-----|-----------|-----|
| 1 | `result` undeclared after `pthread_cleanup_pop` | `pthread_cleanup_push/pop` macros expand to a `{…}` block; variables declared inside go out of scope after `pop` | Declare `int result = 0` **before** `pthread_cleanup_push` |
| 2 | `pipeline_io_mutex` undeclared in `lsh_exit` | Mutex declared inside the pipeline section (after `lsh_exit` in source order) | Moved declaration to the global section alongside `cwd_mutex` |
| 3 | Display prompt buffer overflow warning | `display_prompt[768]` too small for `g_cwd` (4095 bytes) + prompt string | Widened to `display_prompt[4400]` |
| 4 | `help | wc -l` pipeline hangs | Fork child inherited the dup'd write-end fd from the thread stage; never sent EOF to `wc` | Added `thread_fds[]` array; every fork child explicitly closes all thread-dup'd fds |
| 5 | `echo "hello\n" | wc -c` gave 8 not 6 | Shell does not process double-quote escape sequences; `\n` passed as two literal characters | Test changed to `/usr/bin/echo hello | /usr/bin/wc -c` → 6 bytes |
| 6 | Background job timing race with `sleep 0.05` | Fractional sleep too short; thread sometimes not yet in job table when `jobs` ran | Test uses `help &` + `/usr/bin/sleep 0.2` + `jobs` for reliable timing |
| 7 | **Segfault** after `sleep 5 &` then `help &` | Use-after-free: background thread held raw pointer into REPL's `cmds[]`; REPL freed `argv[]` while thread was reading it | `cmd_dup()` deep-copies the `cmd_t` for every background thread; `cmd_free()` releases it in `bg_builtin_cleanup` |

---

## 5. Test Suite — `test_week6.sh`

23 checks across 9 test sections:

| Section | What is tested | Result |
|---------|---------------|--------|
| 1. Basic built-in in thread | `help` output is correct | 3/3 PASS |
| 2. Built-in → Fork pipeline | `help | grep`, `help | wc -l` | 3/3 PASS |
| 3. Fork → Built-in pipeline | `echo | jobs` (no hang), `echo | wc -c` | 3/3 PASS |
| 4. Background built-in (`&`) | `help &` job entry, `jobs &` job entry | 3/3 PASS |
| 5. cd / exit guards | `cd &` and `exit &` rejected with error | 2/2 PASS |
| 6. cd changes directory | `cd /tmp; pwd`, prompt update, failed cd | 3/3 PASS |
| 7. exit code propagation | `exit 0`, `exit 42`, `exit 1` | 3/3 PASS |
| 8. Clean exit with threads | Exit with 3 background threads; mixed proc+thread | 2/2 PASS |
| 9. Rapid sequential cd | Last `cd` wins after 3 rapid changes | 1/1 PASS |

**Final result: 23 / 23 passed**

---

## 6. Key Design Decisions

### Threads are joinable, never detached

Background built-in threads are **not** `pthread_detach`'d. They remain joinable so that:
- `lsh_exit()` can call `pthread_join()` to reclaim the thread stack (no resource leak)
- `jobs_print_and_prune()` can join Done threads to release resources

### Lock-then-join pattern (deadlock prevention)

`lsh_exit()` and `jobs_print_and_prune()` collect thread IDs into a local array **while holding** `job_table_mutex`, then release the lock and join outside it. This prevents deadlock: `bg_builtin_cleanup` also acquires `job_table_mutex` — if the main thread held the lock while blocked in `pthread_join`, the thread could never acquire the lock to mark itself Done, and `pthread_join` would never return.

### SIGCHLD stays in the main thread

All worker threads call `pthread_sigmask(SIG_BLOCK, &mask, SIGCHLD)` at startup so the signal is always delivered to the main thread where the `sigaction` handler lives. The handler writes to `sigchld_pipe`; the reaper thread reads from it and calls `waitpid` under the mutex.

### `FD_CLOEXEC` is not sufficient alone for pipeline fds

`FD_CLOEXEC` closes file descriptors only on `exec()`. Registry built-ins that return without calling `exec()` use `_exit()` instead, which does **not** trigger `FD_CLOEXEC`. The explicit close loop in fork children is therefore mandatory, not optional.

---

## 7. Files Modified

| File | Change |
|------|--------|
| `aishell_main.c` | All changes above: thread dispatch, mutexes, g_cwd, pipeline thread support, cmd_dup/cmd_free, lsh_exit shutdown |
| `test_week6.sh` | New — 23-check pthread integration test suite |
| `Makefile` | Already had `-pthread` in `CFLAGS` and `LDFLAGS` from Week 5 |

---

## 8. Build Instructions

```sh
cd week3
make          # produces ./aishell (linked with -lpthread -largtable3)
make clean    # remove build artifacts
```

## 9. Running the Tests

```sh
cd week3
make
bash test_week6.sh
```

Expected output:
```
==================================================================
 Week 6 — pthread integration test suite
==================================================================
  ...
 Results: 23 / 23 passed
==================================================================
```

---

*Work log prepared for Week 6 of the AiShell project.*
