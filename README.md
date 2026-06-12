# week9+ — 59 Commands, Full Shell Grammar, Arithmetic & Process Substitution

A modular, BusyBox-style shell implemented in C. All 59 commands compile into a **single `aishell` binary** that runs as an interactive REPL or dispatches subcommands directly.

Builds on [week8](https://github.com/jaganath-k/project_aishell) (MCP/AI integration). Week 9–12 extends the shell with 24 new utility commands, full POSIX-style conditional tests (`test` / `[`), stream multiplexing (`xargs`, `tee`, `nc`), checksums, aliases, command history, arithmetic expansion with variable references (`$((x * 2))`), and process substitution (`<(cmd)`, `>(cmd)`).

---

## Quick Start

```sh
# Install dependencies (one-time)
sudo apt-get install -y bnfc flex bison libcurl4-openssl-dev

# Generate parser/lexer from Grammar.cf (run once, or after Grammar.cf changes)
make bnfc-gen

# Build ./aishell
make

# Run the interactive shell
./aishell

# Set OpenRouter API key for AI fallback (get free key at https://openrouter.ai)
export OPENROUTER_API_KEY=sk-or-v1-...
```

---

## Commands (59)

### File & Directory (14)

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
| `ln`    | `-s` symlink, `-f` force | Create hard or symbolic links |
| `chmod` | `MODE FILE` | Change file permissions |
| `chown` | `OWNER[:GROUP] FILE` | Change file owner and group |

### Search & Navigation (2)

| Command | Key Flags | Summary |
|---------|-----------|---------|
| `rg`    | `-i` case, `-F` fixed, `-l` files-only, `-r` recursive, `--json` | Search files with POSIX extended regex |
| `find`  | `-n PATTERN` glob, `-t f/d/l` type, `--maxdepth N` | Recursively search directory tree |

### Text Processing (10)

| Command | Key Flags | Summary |
|---------|-----------|---------|
| `wc`    | `-l` lines, `-w` words, `-c` bytes | Count lines, words, and bytes |
| `sort`  | `-r` reverse, `-n` numeric, `-u` unique, `-k N` field | Sort lines of text |
| `uniq`  | `-c` count, `-d` repeated, `-u` unique, `-i` ignore-case | Filter adjacent duplicate lines |
| `cut`   | `-d DELIM`, `-f LIST` fields, `-c LIST` chars | Remove sections from each line |
| `tr`    | `-d` delete, `-s` squeeze, `-c` complement | Translate or delete characters |
| `grep`  | `-i` case, `-v` invert, `-n` line numbers, `-r` recursive | Print lines matching a pattern |
| `diff`  | — | Compare files line by line |
| `tee`   | `-a` append | Read stdin, write to stdout and files |
| `date`  | `-u` UTC, `-f FMT` strftime | Print current date and time |
| `xargs` | `-n N` batch, `-I {}` replace, `-P N` parallel, `-0` null | Build and execute commands from stdin |

### Disk & System (4)

| Command | Key Flags | Summary |
|---------|-----------|---------|
| `du`    | `-s` summary, `-h` human | Estimate file space usage |
| `df`    | `-h` human | Report file system disk space usage |
| `file`  | — | Determine file type |
| `sleep` | `N[smhd]` | Suspend execution for a time interval |

### Process Management (4)

| Command | Key Flags | Summary |
|---------|-----------|---------|
| `ps`    | `-e` all, `-f` full, `--json` | Report process status |
| `kill`  | `-s SIG`, `-l` list | Send a signal to a process |
| `wait`  | — | Wait for a process to complete |
| `jobs`  | `--json` | List background jobs |

### Shell & Navigation (4)

| Command | Key Flags | Summary |
|---------|-----------|---------|
| `cd`    | — | Change the working directory |
| `pwd`   | — | Print the current working directory |
| `echo`  | `-n` no newline | Display a line of text |
| `which` | — | Locate a command on PATH |

### Environment & Shell Control (7)

| Command | Key Flags | Summary |
|---------|-----------|---------|
| `env`    | `-u NAME`, `NAME=VALUE` | Print environment or set variables |
| `export` | `NAME[=VALUE]` | Set or display variables in `declare -x` format |
| `unset`  | `NAME...` | Remove variables from the environment |
| `type`   | — | Show how a name is interpreted |
| `true`   | — | Return exit status 0 |
| `false`  | — | Return exit status 1 |
| `read`   | `-p PROMPT -s -n N -d DELIM -t N` | Read a line from stdin into a variable |

### Conditionals (2)

| Command | Key Flags | Summary |
|---------|-----------|---------|
| `test`  | `-f -d -e -r -w -x -s -L -z -n`, `=`, `!=`, `-eq -ne -lt -le -gt -ge`, `! -a -o` | Evaluate a conditional expression |
| `[`     | same as test, requires closing `]` | Alias for test used in `if [ ... ]` syntax |

### Checksums (2)

| Command | Summary |
|---------|---------|
| `md5sum`    | Compute MD5 checksum of files or stdin |
| `sha256sum` | Compute SHA-256 checksum of files or stdin |

### Networking (2)

| Command | Key Flags | Summary |
|---------|-----------|---------|
| `ping`  | `-c N` count, `-i N` interval, `-W N` timeout | Send ICMP echo requests |
| `nc`    | `-l` listen, `-p PORT`, `-z` zero-I/O, `-u` UDP | TCP/UDP client and server |

### Session (2)

| Command | Key Flags | Summary |
|---------|-----------|---------|
| `alias`   | `NAME=VALUE` | Define or display command aliases |
| `unalias` | `NAME` | Remove a command alias |
| `history` | `-c` clear, `-n N` show last N | Display or clear command history |

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

## AI / Natural Language Commands (`@`)

Type `@ <natural language query>` to let the shell find and run the right command.

```sh
@ check system uptime
@ list open network connections
@ delete files older than 7 days in /tmp
@ show top cpu processes
@ list   # show all registry entries
@ log    # tail the last 20 calls from aishell_calls.log
```

### Three-Tier Lookup Chain

```
user types "@ <query>"
       │
       ▼
[1] commands.json registry   — keyword match, zero latency
       │ miss
       ▼
[2] MCP server (localhost:9000, TCP)   — sub-ms probe
       │ unavailable
       ▼
[3] OpenRouter AI (HTTPS, libcurl)    — multi-model free → paid fallback
       │
       ▼
  show command + explanation → confirm → execute
```

---

## Build

**Requirements:** `gcc`, `make`, `argtable3`, `bnfc`, `flex`, `bison`, `libcurl` (optional)

```sh
# Install argtable3 (one-time)
git clone https://github.com/argtable/argtable3.git
cmake -B argtable3/build -DCMAKE_BUILD_TYPE=Release argtable3
cmake --build argtable3/build
sudo cmake --install argtable3/build && sudo ldconfig

# Install BNFC toolchain + libcurl (one-time)
sudo apt-get install -y bnfc flex bison libcurl4-openssl-dev

# Build
make bnfc-gen   # generate Lexer.c + Parser.c from Grammar.cf (once, or after Grammar.cf changes)
make            # produces ./aishell
make clean      # remove build artifacts
```

> **Note:** After any `Grammar.cf` change, run `make bnfc-gen` and then manually run
> `cd bnfc && bison -t -pgrammar_ Grammar.y -o Parser.c` to ensure `Bison.h` is regenerated,
> then `make clean && make`.

---

## REPL Features

| Feature | Detail |
|---------|--------|
| Signal handling | SIGINT/SIGQUIT/SIGTSTP ignored in shell; children restore `SIG_DFL` |
| SIGCHLD handler | `waitpid(-1, WNOHANG)` reaps finished children — no zombies |
| Dynamic prompt | Shows current directory `[/tmp] jshell%`; change with `prompt STRING` |
| Pipelines | `cmd1 \| cmd2 \| cmd3` — built-in and external stages mixed freely |
| Background jobs | `cmd &` — processes return `[N] PID` |
| Output redirect | `cmd > file` (truncate), `cmd >> file` (append) |
| Stderr redirect | `cmd 2> file`, `cmd 2>> file`, `cmd &> file` (stdout+stderr) |
| Input redirect | `cmd < file` |
| Command substitution | `echo $(date)`, `ls $(pwd)` — output replaces the expression |
| **Arithmetic expansion** | `$((expr))` — `+` `-` `*` `/` `%`, parentheses, variable references (`$((x * 2))`) |
| **Process substitution** | `diff <(sort a) <(sort b)`, `tee >(gzip > out.gz)` — via `/proc/self/fd/N` |
| Glob wildcards | `*.c`, `*.h`, `?` expanded via `glob(GLOB_NOCHECK)` |
| Quoted strings | `"hello world"` — single argument with space; `$var` expanded inside |
| AND / OR lists | `cmd1 && cmd2`, `cmd1 \|\| cmd2` |
| Negation | `! cmd` — invert exit code |
| Subshell | `( cmd1 ; cmd2 )` — isolated child process |
| Command group | `{ cmd1 ; cmd2 }` — current process |
| if / elif / else / fi | Full conditional with unlimited `elif` branches |
| for / while / until | `for i in LIST; do ...`, `while COND; do ...`, `until COND; do ...` |
| break / continue | Loop control |
| Variable assignment | `x=value` — stored in shell variable store and environment |
| Variable expansion | `$x`, `$?`, `$$`, `${var}` — with `getenv()` fallback |
| **Conditionals** | `test EXPR` / `[ EXPR ]` — file, string, numeric, and logical tests |
| @ NL interface | `@ query` — 3-tier lookup: registry → MCP → OpenRouter AI |
| `--commands-json` | Emits all registered commands as JSON for AI/tooling integration |

---

## Grammar (`bnfc/Grammar.cf`)

### Token types

| Token | Pattern | Purpose |
|-------|---------|---------|
| `ArithExp` | `$((` … `))` | Arithmetic expansion; spaces allowed inside |
| `ProcSubstIn` | `<(` … `)` | Process substitution (read end) |
| `ProcSubstOut` | `>(` … `)` | Process substitution (write end) |
| `Assign` | `NAME=VALUE` | Variable assignment |
| `Word` | alphanum + `-._/:*?~$\`+%,=[]` | Standard shell token |

### Command argument types

`CommandPart` accepts an `Arg` non-terminal that can be any of:

```
WrdArg.        Arg ::= Word ;
ArithArg.      Arg ::= ArithExp ;
PSubstInArg.   Arg ::= ProcSubstIn ;
PSubstOutArg.  Arg ::= ProcSubstOut ;
```

### Grammar hierarchy

```
Input       → [Job]
Job         → Condition | Condition & | Assign
            | "if" Condition "then" [Job] OptElse "fi"
            | "for" Word "in" [Word] "do" [Job] "done"
            | "while" Condition "do" [Job] "done"
            | "until" Condition "do" [Job] "done"
            | "break" | "continue"
OptElse     → (empty) | "else" [Job] | "elif" Condition "then" [Job] OptElse
Condition   → NegCmd | NegCmd "&&" Condition | NegCmd "||" Condition
NegCmd      → CommandLine | "!" NegCmd | "time" NegCmd
            | "(" [Job] ")" | "{" [Job] "}"
CommandLine → Pipeline OptRedir
OptRedir    → (empty) | "2>>" Word | "2>" Word | "&>" Word
            | ">>" Word | ">" Word | "<" Word
Pipeline    → CommandPart | CommandPart "|" Pipeline
CommandPart → Arg [Arg]
```

---

## Usage Examples

```sh
# Arithmetic expansion with variable references
x=5
echo $((x * 2))          # 10
echo $((x * x + 1))      # 26
sleep $((60 * 5))        # sleep 5 minutes

# Process substitution
diff <(sort file1) <(sort file2)
comm <(sort a.txt) <(sort b.txt)
echo hello | tee >(wc -c) >/dev/null

# Conditional tests
test -f Makefile && echo "exists"
[ -d /tmp ] && echo "is a dir"
[ "$x" -gt 3 ] && echo "greater"
if [ -f /etc/hostname ]; then cat /etc/hostname; fi

# xargs
find . -name "*.c" | xargs wc -l
echo "a b c" | xargs -n1 echo
ls *.c | xargs -I{} cp {} /tmp/backup/

# Checksums
md5sum Makefile
echo "test" | sha256sum

# Networking
nc -z localhost 22 && echo "port 22 open"

# Aliases
alias ll='ls -la'
ll /tmp

# Pipelines and redirects
grep -r "TODO" . 2>/dev/null | sort | uniq
ls -la | sort -k5 -n | tail -5
diff <(echo hello) <(echo world)

# AI natural language
@ count lines in all c files
@ show disk usage sorted by size
```

---

## Architecture

Every command follows the **APPANATOMY** pattern:

- **`cmd_spec_t`** (`cmd_spec.h`) — uniform struct: `name`, `summary`, `long_help`, `run()`, `print_usage()`
- **`build_<name>_argtable()`** — shared argtable3 builder; called by both `run` and `print_usage`
- **Registry** (`registry.c`) — flat array of up to 64 `cmd_spec_t*`; dispatch via `find_command(name)->run(argc, argv)`
- **`cmd_help_json.c`** — walks live `arg_hdr_t` entries to emit JSON; always in sync with the argtable

### Source Layout

```
aishell_main.c           — REPL, dispatch, BNFC evaluator, @ handler, arith/process-subst
bnfc/Grammar.cf          — BNF grammar source (single file you author)
bnfc/                    — BNFC-generated files (Absyn.h, Lexer.c, Parser.c, …)
commands.json            — NL command registry
cmd_registry.c/h         — cJSON-based registry loader + keyword matcher
mcp_client.c/h           — non-blocking TCP client to MCP server
aishell_client.c/h       — OpenRouter AI client via libcurl
aishell_log.c/h          — thread-safe append logger
cJSON.c/h                — embedded JSON parser
cmd_<name>.c             — one file per command (59 total)
edit_utils.c/h           — shared file I/O for edit-* commands
hash_utils.c/h           — shared MD5/SHA-256 implementation
cmd_spec.h               — cmd_spec_t definition + registry API
cmd_help_json.c/h        — shared --help-json implementation
registry.c               — register_command / find_command / for_each_command
pkg.c                    — standalone package manager
registry/                — Node.js + Express HTTP registry server
docs/                    — pre-generated --help-json snapshots
test_week5.sh            — 25 checks — process management
test_week6.sh            — 23 checks — pthread integration
test_week7.sh            — 26 checks — BNFC grammar features
test_week8.sh            — 21 checks — AI/registry/MCP integration
test_week9.sh            — 40 checks — new commands, arithmetic, process substitution
```

---

## Test Suites

```sh
bash test_week5.sh   # 25 checks — fork/exec, pipes, redirects, signals
bash test_week6.sh   # 23 / 23 — pthreads, job table, clean shutdown
bash test_week7.sh   # 26 / 26 — grammar, variables, conditions
bash test_week8.sh   # 21 checks — 14 pass offline (7 require live API key / MCP)
bash test_week9.sh   # 40 checks — xargs, read, test/[, nc, arithmetic, process subst
```

---

## JSON Help Docs

Every command supports `--help-json` and `--help`:

```sh
./aishell xargs  --help-json
./aishell read   --help-json
./aishell test   --help-json
./aishell nc     --help-json
./aishell --commands-json | python3 -m json.tool
```

Pre-generated snapshots live in `docs/`. Regenerate all:

```sh
for cmd in ls cat stat head tail cp mv rm mkdir rmdir touch rg ps kill wait \
           jobs cd pwd echo env export unset type wc sort uniq cut tr grep diff \
           tee du df ln chmod chown sleep which true false file date find \
           md5sum sha256sum alias history ping nc xargs read test \
           edit-show edit-replace-line edit-insert-line edit-delete-line edit-replace; do
    ./aishell $cmd --help-json > docs/${cmd}.json 2>/dev/null
done
```
