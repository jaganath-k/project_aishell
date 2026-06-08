# week8 — MCP/AI Integration + Grammar Improvements

A modular, BusyBox-style shell implemented in C. All 35 commands compile into a **single `aishell` binary** that runs as an interactive REPL or dispatches subcommands directly.

Builds on [week7](https://github.com/jaganath-k/project_aishell) (BNFC grammar pipeline). Week 8 adds a three-tier natural-language `@` command interface backed by a local command registry, an MCP server probe, and an OpenRouter AI fallback. The grammar is extended with stderr redirects (`2>`, `2>>`, `&>`), command substitution (`$(…)`), and a wider token charset. Three new text-processing commands (`uniq`, `cut`, `tr`) bring the command count to 35.

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
[1] commands.json registry   — 18 entries, keyword match, zero latency
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

All queries and outcomes are appended to `aishell_calls.log` (thread-safe).

### OpenRouter Model Chain

| Model | Tier | Notes |
|-------|------|-------|
| `google/gemma-4-31b-it:free` | Free | 31B — best free quality |
| `meta-llama/llama-3.3-70b-instruct:free` | Free | 70B — high quality |
| `meta-llama/llama-3.2-3b-instruct:free` | Free | Small, fast |
| `google/gemma-3-4b-it` | Paid | $0.04/1M — cheapest paid |
| `google/gemini-2.5-flash-lite` | Paid | $0.10/1M — reliable fallback |

```sh
export AISHELL_MODEL=google/gemma-4-31b-it:free   # pin a specific model
```

---

## Commands (35)

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

### Text Processing (6)

| Command | Key Flags | Summary |
|---------|-----------|---------|
| `wc`    | `-l` lines, `-w` words, `-c` bytes | Count lines, words, and bytes |
| `sort`  | `-r` reverse, `-n` numeric, `-u` unique, `-k N` field | Sort lines of text |
| `uniq`  | `-c` count, `-d` repeated, `-u` unique, `-i` ignore-case | Filter adjacent duplicate lines |
| `cut`   | `-d DELIM`, `-f LIST` fields, `-c LIST` chars | Remove sections from each line |
| `tr`    | `-d` delete, `-s` squeeze, `-c` complement | Translate or delete characters |
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

| Command | Description |
|---------|-------------|
| `pkg build <src-dir> <out.tar.gz>` | Package source into a distributable archive |
| `pkg install <archive.tar.gz>` | Install from a local archive |
| `pkg list` | Show all installed packages |
| `pkg remove <name>` | Uninstall a package |
| `pkg check-update <name>` | Query registry for newer version |
| `pkg upgrade <name>` | Download and install latest version |

---

## Build

**Requirements:** `gcc`, `make`, `argtable3`, `bnfc`, `flex`, `bison`, `libcurl` (optional — enables AI fallback)

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

The Makefile auto-detects libcurl. Without it the shell builds and runs normally — the AI fallback prints an install hint instead of calling OpenRouter.

---

## REPL Features

| Feature | Detail |
|---------|--------|
| Signal handling | SIGINT/SIGQUIT/SIGTSTP ignored in shell; children restore `SIG_DFL` |
| SIGCHLD handler | `waitpid(-1, WNOHANG)` reaps finished children — no zombies |
| Dynamic prompt | Shows current directory `[/tmp] jshell%`; change with `prompt STRING` |
| Pipelines | `cmd1 \| cmd2 \| cmd3` — built-in and external stages mixed freely |
| Background jobs | `cmd &` — processes return `[N] PID`; built-ins return `[N] (thread)` |
| Output redirect | `cmd > file` (truncate), `cmd >> file` (append) |
| Stderr redirect | `cmd 2> file`, `cmd 2>> file`, `cmd &> file` (stdout+stderr) |
| Input redirect | `cmd < file` |
| Command substitution | `echo $(date)`, `ls $(pwd)` — output replaces the expression |
| Glob wildcards | `*.c`, `*.h`, `?` expanded via `glob(GLOB_NOCHECK)` |
| Quoted strings | `"hello world"` — single argument with space; `$var` expanded inside |
| AND list | `cmd1 && cmd2` — run cmd2 only if cmd1 succeeds |
| OR list | `cmd1 \|\| cmd2` — run cmd2 only if cmd1 fails |
| Negation | `! cmd` / `! pipeline` — invert exit code |
| Subshell | `( cmd1 ; cmd2 )` — isolated child process |
| Group command | `{ cmd1 ; cmd2 }` — current process; changes propagate |
| if / then / elif / else / fi | Full conditional with unlimited `elif` branches |
| Arithmetic expansion | `$((expr))` — `+` `-` `*` `/` `%`, parentheses, variables |
| Variable assignment | `x=value` — stored in shell variable store |
| Variable expansion | `$x`, `$?`, `$$`, `${var}` — with `getenv()` fallback |
| @ NL interface | `@ query` — 3-tier lookup: registry → MCP → OpenRouter AI |
| `--commands-json` | Emits all 35 commands as JSON for AI/tooling integration |

---

## Grammar (week8 additions on top of week7)

### New in week8

| Feature | Grammar / Code |
|---------|---------------|
| Wider `Word` charset | `+`, `%`, `,` added — enables `ps -eo %cpu`, `find -size +10M` |
| `2> FILE` | `ErrRedir` production |
| `2>> FILE` | `ErrAppRedir` production |
| `&> FILE` | `BothRedir` production — redirects stdout+stderr |
| `$(cmd)` | `preprocess_cmd_subst()` — runs before BNFC parse, uses `popen()` |

### Full `OptRedir` productions

```
ErrAppRedir  ::= "2>>" Word
ErrRedir     ::= "2>"  Word
BothRedir    ::= "&>"  Word
AppendRedir  ::= ">>"  Word
OutRedir     ::= ">"   Word
InRedir      ::= "<"   Word
InOutRedir   ::= "<"   Word ">" Word
OutInRedir   ::= ">"   Word "<" Word
```

### BNFC Grammar Pipeline (week8)

```
getline (EINTR-safe)
  → preprocess_arith()      $((expr)) → numeric result
  → preprocess_cmd_subst()  $(cmd)    → popen output (week8)
  → preprocess_quotes()     "hello world" → hello`world
  → psInput(line)           BNFC LR parser → typed AST
  → eval_input()
```

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
OptRedir  → (empty) | "2>>" Word | "2>" Word | "&>" Word
          | ">>" Word | ">" Word | "<" Word
Pipeline  → CommandPart | CommandPart "|" Pipeline
CommandPart → Word [Word]
```

---

## Usage Examples

```sh
# AI natural language (week8)
@ check system uptime
@ list open network connections
@ delete files older than 7 days in /tmp
@ show top cpu processes
@ count lines in all c files

# New text processing commands (week8)
printf "apple\napple\nbanana\n" | ./aishell uniq
printf "apple\napple\nbanana\n" | ./aishell uniq -c
echo "one:two:three" | ./aishell cut -d: -f2
echo "abcdef" | ./aishell cut -c2-4
echo "Hello World" | ./aishell tr 'a-z' 'A-Z'
echo "Hello 123" | ./aishell tr -d '0-9'
echo "hello   world" | ./aishell tr -s ' '

# New redirects (week8)
make 2> build_errors.txt          # stderr to file
make &> build_all.log             # stdout+stderr to file
make 2>> build_errors.txt         # append stderr

# Command substitution (week8)
echo $(date)
ls $(pwd)
echo $(uname -r)

# Grammar features (week7) — inside ./aishell
echo "hello world"
x=hello ; echo $x
echo $((2+3*4))
ls && echo ok
ls NOTEXIST || echo fallback
! /usr/bin/false && echo negated
if ls *.c then echo found fi
( cd /tmp ; pwd )
time ls | /usr/bin/wc -l
./aishell --commands-json
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
aishell_main.c           — REPL, dispatch, BNFC evaluator, @ handler
bnfc/Grammar.cf          — BNF grammar source (single file you author)
bnfc/                    — BNFC-generated files
commands.json            — 18-entry NL command registry (week8)
cmd_registry.c/h         — cJSON-based registry loader + keyword matcher (week8)
mcp_client.c/h           — non-blocking TCP client to MCP server (week8)
aishell_client.c/h       — OpenRouter AI client via libcurl (week8)
aishell_log.c/h          — thread-safe append logger (week8)
cJSON.c/h                — embedded JSON parser
cmd_<name>.c             — one file per command (35 total)
edit_utils.c/h           — shared file I/O for edit-* commands
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
```

---

## Test Suites

```sh
bash test_week5.sh   # 25 / 25 — fork/exec, pipes, redirects, signals
bash test_week6.sh   # 23 / 23 — pthreads, job table, clean shutdown
bash test_week7.sh   # 26 / 26 — grammar, variables, conditions, arithmetic
bash test_week8.sh   # 21 checks — 14 pass offline (7 require live API key / MCP)
```

---

## JSON Help Docs

Every command supports `--help-json` and `--help`:

```sh
./aishell uniq --help-json
./aishell cut  --help-json
./aishell tr   --help-json
./aishell --commands-json | python3 -m json.tool
```

Pre-generated snapshots live in `docs/`. Regenerate all:

```sh
for cmd in ls cat stat head tail cp mv rm mkdir rmdir touch rg ps kill wait \
           jobs cd pwd echo env export unset type wc sort uniq cut tr date \
           find edit-show edit-replace-line edit-insert-line edit-delete-line \
           edit-replace; do
    ./aishell $cmd --help-json > docs/${cmd}.json
done
```
