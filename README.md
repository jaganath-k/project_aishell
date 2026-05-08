# aishell — week3

A modular, BusyBox-style shell implemented in C. All 23 commands compile into a **single `aishell` binary** that runs as an interactive REPL or dispatches subcommands directly.

Builds on [aishell-week2](https://github.com/jaganath-k/aishell-week2), adding a complete command set, glob wildcard expansion, I/O redirection, and a two-phase tokeniser pipeline that mirrors the reference Unix shell architecture.

---

## Quick Start

```sh
# Build
make

# Interactive shell
./aishell

# Subcommand (no separate binaries needed)
./aishell ls .
./aishell stat README.md
./aishell rg "error" test.txt
```

---

## Commands (23)

### File & Directory

| Command | Summary |
|---------|---------|
| `ls`    | List directory contents (`-a` hidden, `-l` long format) |
| `cat`   | Concatenate and print files (`-n` line numbers) |
| `stat`  | Display file status (size, inode, permissions, timestamps) |
| `head`  | Output first N lines of a file (default 10) |
| `tail`  | Output last N lines of a file (default 10) |
| `cp`    | Copy files |
| `mv`    | Move or rename files |
| `rm`    | Remove files |
| `mkdir` | Create directories |
| `rmdir` | Remove empty directories |
| `touch` | Create files or update timestamps |

### Search

| Command | Summary |
|---------|---------|
| `rg`    | Search files for a regex pattern (POSIX extended regex) |

### Process Management

| Command | Summary |
|---------|---------|
| `ps`    | Report process status (reads `/proc`) |
| `kill`  | Send a signal to a process |
| `wait`  | Wait for a process to complete |
| `jobs`  | List processes in the current session |

### Shell & Navigation

| Command | Summary |
|---------|---------|
| `cd`    | Change the working directory |
| `pwd`   | Print the current working directory |
| `echo`  | Display a line of text (`-n` suppresses newline) |

### Environment

| Command | Summary |
|---------|---------|
| `env`    | Print environment; optionally set/unset vars before printing |
| `export` | Set variables (`NAME=VALUE`) or display them (`declare -x` format) |
| `unset`  | Remove variables from the environment |
| `type`   | Show how a name is interpreted (builtin vs registered command) |

### Shell Built-ins (not in registry)

| Command | Behaviour |
|---------|-----------|
| `exit [N]`      | Exit the shell with code N (default 0) |
| `prompt STRING` | Change the prompt string for this session |

---

## Build

**Requirements:** `gcc`, `make`, `argtable3`

Install argtable3 (one-time):
```sh
git clone https://github.com/argtable/argtable3.git
cmake -B argtable3/build -DCMAKE_BUILD_TYPE=Release argtable3
cmake --build argtable3/build
sudo cmake --install argtable3/build
sudo ldconfig
```

Build the single binary:
```sh
make        # produces ./aishell
make clean  # remove build artifacts
```

Build flags: `-Wall -Wextra -std=c11`, linked with `-largtable3`.

---

## REPL Features

| Feature | Detail |
|---------|--------|
| Signal masking | SIGINT, SIGQUIT, SIGTSTP blocked — shell survives Ctrl-C / Ctrl-Z |
| Colored prompt | Yellow ANSI on terminals; plain text when piped |
| Default prompt | `jshell% ` — change with `prompt STRING` |
| Sequential commands | `cmd1 ; cmd2` or `cmd1;cmd2` (spaces optional) |
| Output redirection | `cmd > file` or `cmd>file` |
| Input redirection  | `cmd < file` or `cmd<file` |
| Glob wildcards | `*.c`, `*.h`, `?` expanded via `glob(GLOB_NOCHECK)` |
| Path-prefix strip | `./ls`, `/usr/bin/ls`, and `ls` all resolve to the same command |

---

## Usage Examples

```sh
# File operations
./aishell ls -la .
./aishell head -n 5 test.txt
./aishell rg "error" test.txt

# Inside the REPL
./aishell
jshell% ls *.c
jshell% cat README.md > /tmp/out.txt
jshell% echo one;echo two;echo three
jshell% head -n 3 test.txt

# Environment management
jshell% export MYVAR=hello
jshell% env | rg MYVAR
jshell% unset MYVAR
jshell% type ls
jshell% type exit

# Help
./aishell ls --help
./aishell env --help
./aishell type --help-json
```

---

## Architecture

Every command follows the **APPANATOMY** pattern (see `APPANATOMY.md`):

- **`cmd_spec_t`** (`cmd_spec.h`) — uniform struct: `name`, `summary`, `long_help`, `run()`, `print_usage()`
- **`build_<name>_argtable()`** — shared argtable3 builder with pointer out-parameters; called by both `run` and `print_usage`
- **Registry** (`registry.c`) — flat array of up to 32 `cmd_spec_t*`; dispatch via `find_command(name)->run(argc, argv)`
- **`cmd_help_json.c`** — walks live `arg_hdr_t` entries to emit JSON; always in sync with the argtable

### Dispatch modes (`aishell_main.c`)

```
1. BusyBox   — invoked as a symlink named after a command → basename dispatch
2. Subcommand — ./aishell <cmd> [args...]  → argv shifted, cmd->run() called
3. REPL      — ./aishell  (no args) → interactive shell
```

### Tokeniser pipeline (REPL)

```
getline (EINTR-safe)
  → preprocess()        pad ; < > with spaces
  → tokenise()          flat token[] array       [mirrors Token.c]
  → separate_commands() cmd_t[] structs           [mirrors Command.c]
  → execute_command()   registry dispatch         [mirrors Builtin.c]
```

### Source layout

```
aishell_main.c      — REPL + multi-call dispatch
cmd_<name>.c        — one file per command (23 total)
cmd_spec.h          — cmd_spec_t definition + registry API
cmd_help_json.c/h   — shared --help-json implementation
registry.c          — register_command / find_command / for_each_command
docs/               — pre-generated --help-json snapshots (23 JSON files)
```

---

## JSON Help Docs

Every command supports `--help-json` for machine-readable metadata:

```sh
./aishell ls --help-json
./aishell env --help-json
```

Pre-generated snapshots live in `docs/`. Regenerate all:
```sh
for cmd in ls cat stat head tail cp mv rm mkdir rmdir touch rg ps kill wait jobs \
           cd pwd echo env export unset type; do
    ./aishell $cmd --help-json > docs/${cmd}.json
done
```
