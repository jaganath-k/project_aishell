# week7 — Grammar-Based Parsing (BNF/BNFC)

A modular, BusyBox-style shell implemented in C. All 32 commands compile into a **single `aishell` binary** that runs as an interactive REPL or dispatches subcommands directly.

Builds on [aishell-week6](https://github.com/jaganath-k/project_aishell), replacing the hand-rolled tokeniser with a formal BNF grammar processed by BNFC. The grammar defines shell syntax precisely; BNFC generates a lexer, LR parser, and typed AST automatically. An AST evaluator bridges the typed tree to the existing Week 5/6 execution layer — pipelines, redirection, job table, pthreads — all unchanged. New language features (`&&`, `||`, `!`, `if/elif/else/fi`, subshell, group, arithmetic, `time`, quoted strings) are added as grammar rules.

---

## Quick Start

```sh
# Install BNFC toolchain (one-time)
sudo apt-get install -y bnfc flex bison

# Generate parser/lexer from Grammar.cf (run once, or after Grammar.cf changes)
make bnfc-gen

# Build ./aishell with BNFC parser
make

# Run the interactive shell
./aishell

# Subcommand mode (no separate binaries needed)
./aishell ls .
./aishell wc test.txt

# Emit full command catalog as JSON
./aishell --commands-json
```

---

## Commands (32)

### File & Directory (11)

| Command | Key Flags | Summary |
|---------|-----------|---------|
| `ls`    | `-a` hidden, `-l` long | List directory contents |
| `cat`   | `-n` line numbers | Concatenate and print files |
| `stat`  | `-c` compact, `-f FMT` | Display file status |
| `head`  | `-n N` | Output first N lines (default 10) |
| `tail`  | `-n N` | Output last N lines (default 10) |
| `cp`    | `-r` recursive, `-i` interactive | Copy files |
| `mv`    | `-i` interactive | Move or rename files |
| `rm`    | `-r`, `-i`, `-f` | Remove files |
| `mkdir` | `-p` parents | Create directories |
| `rmdir` | `-p` parents | Remove empty directories |
| `touch` | — | Create files or update timestamps |

### Search & Navigation (2)

| Command | Key Flags | Summary |
|---------|-----------|---------|
| `rg`    | `-i` case, `-F` fixed, `-l` files-only, `-r` recursive, `--json` | Search files with POSIX extended regex |
| `find`  | `-n PATTERN` glob, `-t f/d/l` type, `--maxdepth N` | Recursively search directory tree |

### Text Processing (3)

| Command | Key Flags | Summary |
|---------|-----------|---------|
| `wc`    | `-l` lines, `-w` words, `-c` bytes | Count lines, words, and bytes |
| `sort`  | `-r` reverse, `-n` numeric, `-u` unique, `-k N` field | Sort lines of text |
| `date`  | `-u` UTC, `-f FMT` strftime | Print current date and time |

### Process Management (4)

| Command | Key Flags | Summary |
|---------|-----------|---------|
| `ps`    | `-e` all, `-f` full, `--json` | Report process status |
| `kill`  | `-s SIG`, `-l` list | Send a signal to a process |
| `wait`  | — | Wait for a process to complete |
| `jobs`  | `--json` | List background jobs |

### Shell & Navigation (3)

| Command | Key Flags | Summary |
|---------|-----------|---------|
| `cd`    | — | Change the working directory |
| `pwd`   | — | Print the current working directory |
| `echo`  | `-n` no newline | Display a line of text |

### Environment (4)

| Command | Key Flags | Summary |
|---------|-----------|---------|
| `env`    | `-u NAME`, `NAME=VALUE` | Print environment or set variables |
| `export` | `NAME[=VALUE]` | Set or display variables in `declare -x` format |
| `unset`  | `NAME...` | Remove variables from the environment |
| `type`   | — | Show how a name is interpreted |

### File Editing (5)

| Command | Key Flags | Summary |
|---------|-----------|---------|
| `edit-show`         | `-n N`, `-e E` | Display file with line numbers |
| `edit-replace-line` | `-n N`, `-t TEXT` | Overwrite line N |
| `edit-insert-line`  | `-n N`, `-a`, `-t TEXT` | Insert before (or after with `-a`) line N |
| `edit-delete-line`  | `-n N`, `-e E` | Delete line N or range N–E |
| `edit-replace`      | `-p PAT`, `-r TEXT`, `-g`, `-i` | Regex find and replace |

### Shell Built-ins (not in registry)

| Command | Behaviour |
|---------|-----------|
| `exit [N]`      | Exit the shell with code N (default 0) |
| `prompt STRING` | Change the prompt string for this session |

---

## Package Manager (`pkg`)

`pkg` is a standalone binary managing the full lifecycle of aishell packages.

### pkg commands

| Command | Description |
|---------|-------------|
| `pkg build <src-dir> <out.tar.gz>` | Package source into a distributable archive |
| `pkg install <archive.tar.gz>` | Install from a local archive |
| `pkg list` | Show all installed packages |
| `pkg remove <name>` | Uninstall a package |
| `pkg check-update <name>` | Query registry for newer version |
| `pkg upgrade <name>` | Download and install latest version |

### Build pkg

```sh
gcc -Wall -Wextra -std=c11 -o pkg pkg.c
```

---

## Registry Server

A minimal Node.js + Express HTTP server acting as the package source of truth.

```sh
cd registry && npm install && node server.js
# → http://localhost:3000
```

| Endpoint | Description |
|----------|-------------|
| `GET /packages` | List all available packages |
| `GET /packages/:name` | Get metadata for a single package |
| `GET /files/<archive>.tar.gz` | Download a package archive |

---

## Build

**Requirements:** `gcc`, `make`, `argtable3`, `bnfc`, `flex`, `bison`

**Install argtable3** (one-time):
```sh
git clone https://github.com/argtable/argtable3.git
cmake -B argtable3/build -DCMAKE_BUILD_TYPE=Release argtable3
cmake --build argtable3/build
sudo cmake --install argtable3/build && sudo ldconfig
```

**Install BNFC toolchain** (one-time):
```sh
sudo apt-get install -y bnfc flex bison
```

**Build:**
```sh
make bnfc-gen   # generate Lexer.c + Parser.c from Grammar.cf
make            # produces ./aishell  (-DUSE_BNFC -Ibnfc)
make clean      # remove build artifacts
make bnfc-test  # build TestGrammar AST printer (grammar debugging)
make bnfc-clean # remove BNFC generated object files
```

Build flags: `-Wall -Wextra -std=c11 -pthread -DUSE_BNFC -Ibnfc`, linked with `-largtable3 -pthread`.

---

## REPL Features

| Feature | Detail |
|---------|--------|
| Signal handling | SIGINT/SIGQUIT/SIGTSTP ignored in shell; children restore `SIG_DFL` |
| SIGCHLD handler | `waitpid(-1, WNOHANG)` reaps finished children — no zombies |
| Dynamic prompt | Shows current directory `[/tmp] jshell%`; change with `prompt STRING` |
| Pipelines | `cmd1 \| cmd2 \| cmd3` — built-in and external stages mixed freely |
| Background jobs | `cmd &` — processes return `[N] PID`; built-ins return `[N] (thread)` |
| Output redirect | `cmd > file` (truncate) or `cmd >> file` (append) |
| Input redirect | `cmd < file` |
| Glob wildcards | `*.c`, `*.h`, `?` expanded via `glob(GLOB_NOCHECK)` |
| Quoted strings | `"hello world"` — single argument with space; `$var` expanded inside |
| AND list | `cmd1 && cmd2` — run cmd2 only if cmd1 succeeds |
| OR list | `cmd1 \|\| cmd2` — run cmd2 only if cmd1 fails |
| Negation | `! cmd` / `! pipeline` — invert exit code |
| Subshell | `( cmd1 ; cmd2 )` — isolated child process; changes don't affect parent |
| Group command | `{ cmd1 ; cmd2 }` — current process; changes propagate to parent |
| if / then / elif / else / fi | Full conditional with unlimited `elif` branches |
| Arithmetic expansion | `$((expr))` — `+` `-` `*` `/` `%`, parentheses, variables |
| Pipeline negation | `! cmd1 \| cmd2` — negates the full pipeline exit code |
| time command | `time cmd` / `time pipeline` — prints real elapsed time to stderr |
| Variable assignment | `x=value` — stored in shell variable store |
| Variable expansion | `$x`, `$?`, `$$`, `${var}` — with `getenv()` fallback |
| @ NL handler | `@query` — sends to `mysh_llm`, shows suggestion, runs on confirmation |
| `--commands-json` | Emits all 32 commands as JSON for AI/tooling integration |

---

## Grammar Model (week7)

| Aspect | Detail |
|--------|--------|
| Grammar file | `bnfc/Grammar.cf` — 30 rules; single source of truth for shell syntax |
| Toolchain | BNFC 2.9.5 → Flex + Bison → `Lexer.c` + `Parser.c` + `Absyn.h` |
| Parser type | LR parser generated by Bison; rejects invalid syntax with position info |
| Entry point | `psInput(const char*)` — parses one REPL line into a typed AST |
| Pre-processing | `preprocess_arith()` → `preprocess_quotes()` before the parser |
| Evaluator | `eval_input → eval_job → eval_condition → eval_negcmd → eval_cmdline` |
| Execution | Delegates to Week 5/6: `bnfc_run_cmd()`, `run_pipeline()`, unchanged |
| Variable store | 64-slot `var_store[]`; `$?`, `$$`, `${var}`; falls back to `getenv()` |
| Arithmetic | Recursive descent: `$((expr))` — `+`, `-`, `*`, `/`, `%`, parens, variables |

### Grammar Hierarchy

```
Input     → [Job]
Job       → Condition | Condition & | Assign
          | "if" Condition "then" [Job] OptElse "fi"
OptElse   → (empty) | "else" [Job] | "elif" Condition "then" [Job] OptElse
Condition → NegCmd | NegCmd "&&" Condition | NegCmd "||" Condition
NegCmd    → CommandLine | "!" NegCmd | "time" NegCmd
          | "(" [Job] ")" | "{" [Job] "}"
CommandLine → Pipeline OptRedir
OptRedir  → (empty) | ">>" Word | ">" Word | "<" Word
          | "<" Word ">" Word | ">" Word "<" Word
Pipeline  → CommandPart | CommandPart "|" Pipeline
CommandPart → Word [Word]
```

---

## Thread Model (week6)

| Aspect | Detail |
|--------|--------|
| Dispatch | Every built-in runs in a `pthread` — foreground joined immediately; background left running |
| Job table | Tracks both `JOB_TYPE_PROCESS` (fork) and `JOB_TYPE_THREAD` (pthread) |
| Background threads | Kept joinable; `lsh_exit()` cancels and joins all before exit |
| Cancellation | `PTHREAD_CANCEL_DEFERRED` — safe at syscall points |
| Cleanup handler | `bg_builtin_cleanup` via `pthread_cleanup_push` — runs on cancel or exit |
| `pipeline_io_mutex` | Serialises `dup2 → run → restore` for built-in pipeline stages |
| `g_cwd` + `cwd_mutex` | Mutex-protected cwd buffer updated after every `cd` |
| Clean exit | Cancel + join threads → reap processes → destroy mutexes → `exit()` |

---

## Usage Examples

```sh
# Grammar features (week7) — inside ./aishell
echo "hello world"                          # quoted string
x=hello ; echo $x                           # variable assignment + expansion
echo $((2+3*4))                             # arithmetic → 14
sq=$((7*7)) ; echo $sq                      # assign from arithmetic
ls && echo ok                               # AND list
ls NOTEXIST || echo fallback               # OR list
! /usr/bin/false && echo negated            # NOT
if ls *.c then echo found fi               # if/then/fi
if /usr/bin/false then echo a elif echo ok then echo b else echo c fi
( cd /tmp ; pwd )                           # subshell — parent dir unchanged
{ cd /tmp ; pwd }                           # group — parent dir changes
time ls | /usr/bin/wc -l                   # time pipeline
@list all c files                           # @ natural language query
./aishell --commands-json                   # 32-command JSON catalog

# Thread integration (week6) — inside ./aishell
help &                                      # background built-in thread
help | /usr/bin/grep exit                  # thread → fork pipeline
cd /tmp                                     # prompt updates to [/tmp] jshell%

# Process management (week5) — inside ./aishell
/usr/bin/sleep 5 &                          # background process
ls | /usr/bin/sort | /usr/bin/head -5       # multi-stage pipeline
echo hello > /tmp/out.txt                   # output redirect
wc -l < /tmp/out.txt                        # input redirect
ls *.c | wc -l                              # glob + pipe
```

---

## Architecture

Every command follows the **APPANATOMY** pattern:

- **`cmd_spec_t`** (`cmd_spec.h`) — uniform struct: `name`, `summary`, `long_help`, `run()`, `print_usage()`
- **`build_<name>_argtable()`** — shared argtable3 builder; called by both `run` and `print_usage`
- **Registry** (`registry.c`) — flat array of up to 64 `cmd_spec_t*`; dispatch via `find_command(name)->run(argc, argv)`
- **`cmd_help_json.c`** — walks live `arg_hdr_t` entries to emit JSON; always in sync with the argtable

### Dispatch Modes

```
1. BusyBox    — invoked as a symlink named after a command → basename dispatch
2. Subcommand — ./aishell <cmd> [args...]  → argv shifted, cmd->run() called
3. --commands-json — emit full command catalog as JSON and exit
4. REPL       — ./aishell  (no args) → interactive BNFC-powered shell
```

### BNFC Grammar Pipeline (week7)

```
getline (EINTR-safe)
  → preprocess_arith()   $((expr)) → numeric result
  → preprocess_quotes()  "hello world" → hello`world
  → psInput(line)        BNFC LR parser → typed AST
  → eval_input()
      eval_job()         FG / BG / Assign / IfStmt
      eval_condition()   && / || short-circuit
      eval_negcmd()      ! / time / ( subshell ) / { group }
      eval_cmdline()     cmd_t → bnfc_run_cmd / run_pipeline
```

### Process Management (week5)

```
execute_command()
  ├── pipe_next? → run_pipeline()
  │     ├── SIGCHLD blocked during setup
  │     ├── fflush(NULL) before first fork
  │     ├── each child: SIG_DFL → dup2 → exec
  │     └── parent: close pipes → waitpid loop
  └── single command → lsh_execute()
        ├── builtin  → run in parent
        ├── registry → dup/restore fds + run
        └── external → lsh_launch(): fork → execvp
```

### Thread Dispatch (week6)

```
lsh_execute()
  ├── background built-in?
  │     ├── cmd_dup()                  deep-copy argv (race-free)
  │     ├── jobs_reserve_thread_slot   pre-reserve job slot
  │     ├── pthread_create(bg_builtin_thread_fn)
  │     │     ├── pthread_cleanup_push(bg_builtin_cleanup)
  │     │     ├── run built-in
  │     │     └── pthread_cleanup_pop → mark Done, free
  │     └── jobs_set_thread_tid
  └── foreground built-in?
        ├── pthread_create(builtin_thread_entry)
        └── pthread_join

lsh_exit()
  ├── collect tids → pthread_cancel → pthread_join
  ├── waitpid(-1, WNOHANG) loop
  ├── pthread_mutex_destroy × 3
  └── exit(code)
```

### Source Layout

```
aishell_main.c           — REPL + dispatch + BNFC evaluator (#ifdef USE_BNFC)
bnfc/Grammar.cf          — BNF grammar source (30 rules) — only file you author
bnfc/                    — BNFC-generated files (excluded from git via .gitignore)
mysh_llm                 — Python AI helper for @ natural language queries
cmd_<name>.c             — one file per command (32 total)
edit_utils.c/h           — shared file I/O for edit-* commands
cmd_spec.h               — cmd_spec_t definition + registry API
cmd_help_json.c/h        — shared --help-json implementation
registry.c               — register_command / find_command / for_each_command
pkg.c                    — standalone package manager (6 subcommands)
pkg.json                 — aishell package descriptor
registry/                — Node.js + Express HTTP registry server
docs/                    — pre-generated --help-json snapshots (32 JSON files)
test_week5.sh            — 25 checks — process management
test_week6.sh            — 23 checks — pthread integration
test_week7.sh            — 26 checks — BNFC grammar features
```

---

## Test Suites

```sh
bash test_week5.sh   # 25 / 25 — fork/exec, pipes, redirects, signals
bash test_week6.sh   # 23 / 23 — pthreads, job table, clean shutdown
bash test_week7.sh   # 26 / 26 — grammar, variables, conditions, arithmetic
```

---

## JSON Help Docs

Every command supports `--help-json` and `--help`:

```sh
./aishell wc --help-json
./aishell sort --help
./aishell --commands-json | python3 -m json.tool
```

Pre-generated snapshots live in `docs/`. Regenerate all:

```sh
for cmd in ls cat stat head tail cp mv rm mkdir rmdir touch rg ps kill wait \
           jobs cd pwd echo env export unset type wc sort date find edit-show \
           edit-replace-line edit-insert-line edit-delete-line edit-replace; do
    ./aishell $cmd --help-json > docs/${cmd}.json
done
```
