# aishell — week4

A modular, BusyBox-style shell implemented in C. All 32 commands compile into a **single `aishell` binary** that runs as an interactive REPL or dispatches subcommands directly.

Builds on [aishell-week2](https://github.com/jaganath-k/aishell-week2), adding a complete command set, glob wildcard expansion, I/O redirection, file editing, and a two-phase tokeniser pipeline that mirrors the reference Unix shell architecture.

---

## Quick Start

```sh
# Build
make

# Interactive shell
./aishell

# Subcommand (no separate binaries needed)
./aishell ls .
./aishell wc test.txt
./aishell rg "error" test.txt
```

---

## Commands (32)

### File & Directory (11)

| Command | Key Flags | Summary |
|---------|-----------|---------|
| `ls`    | `-a` hidden, `-l` long | List directory contents |
| `cat`   | `-n` line numbers | Concatenate and print files |
| `stat`  | `-c` compact, `-f FMT` | Display file status (size, inode, permissions, timestamps) |
| `head`  | `-n N` | Output first N lines of a file (default 10) |
| `tail`  | `-n N` | Output last N lines of a file (default 10) |
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
| `wc`    | `-l` lines, `-w` words, `-c` bytes | Count lines, words, and bytes in files |
| `sort`  | `-r` reverse, `-n` numeric, `-u` unique, `-k N` by field | Sort lines of text files |
| `date`  | `-u` UTC, `-f FMT` strftime format | Print the current date and time |

### Process Management (4)

| Command | Key Flags | Summary |
|---------|-----------|---------|
| `ps`    | `-e` all, `-f` full, `--json` | Report process status |
| `kill`  | `-s SIG`, `-l` list | Send a signal to a process |
| `wait`  | — | Wait for a process to complete |
| `jobs`  | `--json` | List processes in the current session |

### Shell & Navigation (3)

| Command | Key Flags | Summary |
|---------|-----------|---------|
| `cd`    | — | Change the working directory |
| `pwd`   | — | Print the current working directory |
| `echo`  | `-n` no newline | Display a line of text |

### Environment (4)

| Command | Key Flags | Summary |
|---------|-----------|---------|
| `env`    | `-u NAME`, `NAME=VALUE` | Print environment or set variables for display |
| `export` | `NAME[=VALUE]` | Set variables or display them in `declare -x` format |
| `unset`  | `NAME...` | Remove variables from the environment |
| `type`   | — | Show how a name is interpreted (builtin vs registered command) |

### File Editing (5)

| Command | Key Flags | Summary |
|---------|-----------|---------|
| `edit-show`         | `-n N`, `-e E` | Display a file with line numbers (optionally a range) |
| `edit-replace-line` | `-n N`, `-t TEXT` | Overwrite line N with new text |
| `edit-insert-line`  | `-n N`, `-a`, `-t TEXT` | Insert a line before (or after with `-a`) line N |
| `edit-delete-line`  | `-n N`, `-e E` | Delete line N or a range N–E |
| `edit-replace`      | `-p PAT`, `-r TEXT`, `-g`, `-i` | Regex find and replace across all lines |

### Shell Built-ins (not in registry)

| Command | Behaviour |
|---------|-----------|
| `exit [N]`      | Exit the shell with code N (default 0) |
| `prompt STRING` | Change the prompt string for this session |

---

## Package Manager (`pkg`)

`pkg` is a standalone binary that manages the full lifecycle of aishell packages. It is separate from the shell binary but integrates with the REPL — typing `pkg ...` inside aishell dispatches to the `pkg` binary automatically.

### Install layout

```
~/.mysh/
├── bin/               ← symlinks to installed executables (prepended to PATH)
├── pkgs/
│   └── <name>-<ver>/  ← extracted package files
└── pkgdb.txt          ← installed package database (name version, one per line)
```

### pkg commands

| Command | Description |
|---------|-------------|
| `pkg build <src-dir> <out.tar.gz>` | Package a source directory into a distributable archive |
| `pkg install <archive.tar.gz>` | Install a package from a local archive |
| `pkg list` | Show all installed packages |
| `pkg remove <name>` | Uninstall a package (removes symlinks and files) |
| `pkg check-update <name>` | Query the registry and report if a newer version is available |
| `pkg upgrade <name>` | Download and install the latest version from the registry |

### pkg.json — package metadata

Every package ships a `pkg.json` at the root of its source directory:

```json
{
  "name": "aishell",
  "version": "2.0",
  "description": "BusyBox-style shell with 32 built-in commands",
  "files": ["aishell"],
  "docs": ["README.md"]
}
```

- `files` — executables to symlink into `~/.mysh/bin/`
- `version` — compared numerically (dot-separated) during `check-update`

### Build pkg

```sh
gcc -Wall -Wextra -std=c11 -o pkg pkg.c
```

### Usage examples

```sh
# Build a package from source
./pkg build . ~/aishell-2.0.tar.gz

# Install from a local archive
./pkg install ~/aishell-2.0.tar.gz

# List installed packages
./pkg list
# NAME                     VERSION
# ------------------------ -------
# aishell                  2.0

# Check if an update is available
./pkg check-update aishell
# Update available: aishell  1.0 -> 2.0
#   Run: pkg upgrade aishell

# Upgrade to the latest version from the registry
./pkg upgrade aishell
# pkg: upgrading aishell  1.0 -> 2.0
# pkg: downloading http://localhost:3000/files/aishell-2.0.tar.gz ...
# pkg: aishell-2.0 installed successfully

# Remove a package
./pkg remove aishell
```

### Using pkg inside the aishell REPL

`~/.mysh/bin` is prepended to `$PATH` at shell startup, so installed packages and the `pkg` binary itself are available as first-class shell commands:

```sh
./aishell
jshell% pkg list
jshell% pkg check-update aishell
jshell% pkg upgrade aishell
```

---

## Registry Server

The registry is a minimal Node.js + Express HTTP server that acts as the source of truth for available packages and download URLs.

### Setup

```sh
cd registry
npm install        # first time only
node server.js
# aishell registry  →  http://localhost:3000
```

### Endpoints

| Endpoint | Description |
|----------|-------------|
| `GET /packages` | List all available packages |
| `GET /packages/:name` | Get metadata for a single package |
| `GET /files/<archive>.tar.gz` | Download a package archive |

```sh
curl -s http://localhost:3000/packages/aishell
# {"name":"aishell","latestVersion":"2.0","downloadUrl":"http://localhost:3000/files/aishell-2.0.tar.gz",...}
```

### Adding a package to the registry

1. Build the archive: `./pkg build <src-dir> registry/files/<name>-<ver>.tar.gz`
2. Add an entry to `packages[]` in `registry/server.js`
3. Restart the server: `node server.js`

### Registry layout

```
registry/
├── server.js          ← Express server (in-memory package list)
├── package.json       ← npm metadata
└── files/             ← .tar.gz archives served statically (gitignored)
```

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
./aishell wc test.txt
./aishell wc -l *.c

# Search and filter
./aishell rg "error" test.txt
./aishell find . -n "*.c" -t f
./aishell find . --maxdepth 1 -t d

# Text processing
./aishell sort test.txt
./aishell sort -n -k 2 test.txt        # numeric sort on field 2
./aishell sort -r -u test.txt          # reverse, remove duplicates
./aishell date
./aishell date -f "%Y-%m-%d"           # ISO date
./aishell date -u                      # UTC

# File editing (preview first, then edit)
./aishell edit-show test.txt           # view all lines with numbers
./aishell edit-show -n 8 -e 10 test.txt
./aishell edit-replace-line -n 4 -t "new content" test.txt
./aishell edit-insert-line -n 3 -t "inserted line" test.txt
./aishell edit-insert-line -n 3 -a -t "after line 3" test.txt
./aishell edit-delete-line -n 5 test.txt
./aishell edit-delete-line -n 5 -e 8 test.txt
./aishell edit-replace -p "error" -r "[NOTICE]" -g -i test.txt

# Inside the REPL
./aishell
jshell% ls *.c
jshell% wc -l *.c
jshell% sort test.txt | head -5
jshell% find . -n "*.h" -t f
jshell% cat README.md > /tmp/out.txt
jshell% echo one;echo two;echo three

# Environment management
jshell% export MYVAR=hello
jshell% env | rg MYVAR
jshell% unset MYVAR
jshell% type ls
jshell% type exit

# Help
./aishell wc --help
./aishell sort --help
./aishell find --help
./aishell edit-show --help-json

# Package management (start registry server first)
./pkg build . ~/aishell-2.0.tar.gz
./pkg install ~/aishell-2.0.tar.gz
./pkg list
./pkg check-update aishell
./pkg upgrade aishell
./pkg remove aishell
```

---

## Architecture

Every command follows the **APPANATOMY** pattern (see `APPANATOMY.md`):

- **`cmd_spec_t`** (`cmd_spec.h`) — uniform struct: `name`, `summary`, `long_help`, `run()`, `print_usage()`
- **`build_<name>_argtable()`** — shared argtable3 builder with pointer out-parameters; called by both `run` and `print_usage`
- **Registry** (`registry.c`) — flat array of up to 64 `cmd_spec_t*`; dispatch via `find_command(name)->run(argc, argv)`
- **`cmd_help_json.c`** — walks live `arg_hdr_t` entries to emit JSON; always in sync with the argtable
- **`edit_utils.c`** — shared line-oriented file I/O for all `edit-*` commands (atomic write via temp + rename)

### Dispatch modes (`aishell_main.c`)

```
1. BusyBox    — invoked as a symlink named after a command → basename dispatch
2. Subcommand — ./aishell <cmd> [args...]  → argv shifted, cmd->run() called
3. REPL       — ./aishell  (no args) → interactive shell
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
aishell_main.c           — REPL + multi-call dispatch + pkg integration
cmd_<name>.c             — one file per command (32 total)
edit_utils.c/h           — shared file I/O helpers for edit-* commands
cmd_spec.h               — cmd_spec_t definition + registry API
cmd_help_json.c/h        — shared --help-json implementation
registry.c               — register_command / find_command / for_each_command (cap: 64)
pkg.c                    — standalone package manager binary (6 subcommands)
pkg.json                 — aishell package descriptor
registry/                — Node.js + Express HTTP registry server
docs/                    — pre-generated --help-json snapshots (32 JSON files)
```

---

## JSON Help Docs

Every command supports `--help-json` for machine-readable metadata:

```sh
./aishell wc --help-json
./aishell sort --help-json
./aishell find --help-json
```

Pre-generated snapshots live in `docs/`. Regenerate all:
```sh
for cmd in ls cat stat head tail cp mv rm mkdir rmdir touch rg ps kill wait jobs \
           cd pwd echo env export unset type \
           edit-replace-line edit-insert-line edit-delete-line edit-replace \
           wc sort date find edit-show; do
    ./aishell $cmd --help-json > docs/${cmd}.json
done
```
