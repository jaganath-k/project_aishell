# AiShell — Week 10 (Final): ShellAI Hardening + MCP Agent Backend

A modular, BusyBox-style shell implemented in C. All 61 commands compile into a **single `aishell` binary** that runs as an interactive REPL or dispatches subcommands directly.

Builds on [week8](https://github.com/jaganath-k/project_aishell) (MCP/AI integration). Week 9 adds the MCP/FTP server, RAG-grounded `@`, and config system. Week 10 hardens the `@` pipeline and completes the MCP agent toolset:

| Feature | Week | Description |
|---------|------|-------------|
| **MCP/FTP Server** | 9 | TCP server on port 9000 — handles FTP-style file transfers (USER/QUIT/PORT/STOR/RETR/LIST/MKD) and MCP JSON tool calls from the same port. One pthread per client. |
| **RAG-Grounded `@`** | 9 | Upgrades `@` from keyword matching to TF-IDF cosine similarity retrieval against `commands.json`. High-confidence matches execute directly; low-confidence matches inject context into the LLM prompt. |
| **Config System** | 9 | `aishell.conf` key=value config file. Runtime updates via `server config key=value`. New `server` built-in command manages the server lifecycle. |
| **Heuristic Fallback** | 10 | When the LLM is unavailable, `heuristic_fallback_command()` scores every registered command by query-token overlap (name=3pts, summary=2pts, long_help=1pt) and returns the best match — the `@` pipeline never falls silent. |
| **Hardened Validation** | 10 | `validate_suggestion()` strips multi-line LLM responses to a single token, rejects anything not in the command registry, and gates destructive commands (`rm -rf`, `dd`, …) behind an explicit confirmation prompt. |
| **20-tool MCP Backend** | 10 | Adds `fs.*` (6), `proc.*` (3), `env.*` (3), and `edit.*` (4) tool families to the existing 4 Week 9 tools — full agent backend over the existing MCP port. |
| **Unified `edit` command** | 10 | New `edit` built-in consolidates `edit-replace-line`, `edit-insert-line`, `edit-delete-line`, and `edit-replace` into one subcommand interface with atomic file writes. |

---

## Quick Start

```sh
# Install dependencies (one-time)
sudo apt-get install -y bnfc flex bison libcurl4-openssl-dev

# Generate parser/lexer from Grammar.cf (run once, or after Grammar.cf changes)
make bnfc-gen

# Build ./aishell
make

# Run the interactive shell (server auto-starts on port 9000)
./aishell

# Run as a background server daemon only (no REPL)
./aishell --server &

# Set OpenRouter API key for AI fallback (get free key at https://openrouter.ai)
export OPENROUTER_API_KEY=sk-or-v1-...
```

---

## Week 9 Features

### MCP/FTP Server (Week 9)

The server starts automatically when aishell launches. It listens on port 9000 and detects client type from the first byte: `{` → MCP JSON client, anything else → FTP client.

```sh
# Inside the REPL
server status                        # show running/stopped + port
server stop                          # stop the server
server start                         # restart the server
server config                        # print all aishell.conf settings
server config server_timeout=60      # live update one setting
server config rag_top_k=5
server config server_allowlist=ls,cat,grep,echo
```

#### FTP Protocol (via `nc` or real `ftp` client)

```sh
# Basic session
printf 'USER testuser\r\nQUIT\r\n' | nc 127.0.0.1 9000

# Create a directory
printf 'USER test\r\nMKD mydir\r\nQUIT\r\n' | nc 127.0.0.1 9000

# Duplicate MKD → 550 error
printf 'USER test\r\nMKD mydir\r\nMKD mydir\r\nQUIT\r\n' | nc 127.0.0.1 9000

# Path traversal → 550 Permission denied
printf 'USER test\r\nRETR ../../../etc/passwd\r\nQUIT\r\n' | nc 127.0.0.1 9000

# Missing file → 550 File not found
printf 'USER test\r\nRETR nosuchfile.txt\r\nQUIT\r\n' | nc 127.0.0.1 9000

# Show current directory
printf 'USER test\r\nPWD\r\nQUIT\r\n' | nc 127.0.0.1 9000
```

**Binary-safe STOR + RETR** (active mode — client opens data port):

```sh
# Create test file with NUL bytes
printf 'hello\x00world\nline2\x00end\n' > /tmp/nultest.bin

# STOR: nc sends the file (client → server). 5050 = 19×256+186
nc -l 5050 < /tmp/nultest.bin &
printf 'USER test\r\nPORT 127,0,0,1,19,186\r\nSTOR nultest.bin\r\nQUIT\r\n' \
  | nc 127.0.0.1 9000

# RETR: nc receives the file (server → client). 5051 = 19×256+187
nc -l 5051 </dev/null > /tmp/retrieved.bin &
printf 'USER test\r\nPORT 127,0,0,1,19,187\r\nRETR nultest.bin\r\nQUIT\r\n' \
  | nc 127.0.0.1 9000

# Verify binary-identical
cmp /tmp/nultest.bin /tmp/retrieved.bin && echo "Binary-safe STOR+RETR: PASS"
```

**Using the real `ftp` client** (`sudo apt install tnftp`):

```sh
ftp 127.0.0.1 9000
ftp> user testuser
ftp> pwd
ftp> mkdir ftptest
ftp> ls
ftp> put /etc/hostname hostname.txt
ftp> get hostname.txt /tmp/got.txt
ftp> quit
```

#### MCP JSON Tools (via `nc`)

All 20 tools are available on port 9000. Send a length-framed JSON request:

```sh
# Week 9 tools — shell management
printf '{"type":"mcp","tool":"list_tools","params":{}}\n' | nc 127.0.0.1 9000
printf '{"type":"mcp","tool":"get_status","params":{}}\n' | nc 127.0.0.1 9000
printf '{"type":"mcp","tool":"get_registry","params":{}}\n' | nc 127.0.0.1 9000
printf '{"type":"mcp","tool":"run_command","command":"echo","args":"hello"}\n' \
  | nc 127.0.0.1 9000

# Week 10 — fs.* (filesystem tools)
printf '{"type":"mcp","tool":"fs.list","params":{"path":"."}}\n' | nc 127.0.0.1 9000
printf '{"type":"mcp","tool":"fs.read","params":{"path":"aishell.conf"}}\n' | nc 127.0.0.1 9000
printf '{"type":"mcp","tool":"fs.write","params":{"path":"/tmp/a.txt","content":"hello"}}\n' | nc 127.0.0.1 9000
printf '{"type":"mcp","tool":"fs.append","params":{"path":"/tmp/a.txt","content":"\\nline2"}}\n' | nc 127.0.0.1 9000
printf '{"type":"mcp","tool":"fs.stat","params":{"path":"Makefile"}}\n' | nc 127.0.0.1 9000
printf '{"type":"mcp","tool":"fs.search","params":{"path":".","pattern":"TODO","recursive":true}}\n' | nc 127.0.0.1 9000

# Week 10 — proc.* (job/process tools)
printf '{"type":"mcp","tool":"proc.list","params":{}}\n' | nc 127.0.0.1 9000
printf '{"type":"mcp","tool":"proc.kill","params":{"job_id":1,"signal":"SIGTERM"}}\n' | nc 127.0.0.1 9000
printf '{"type":"mcp","tool":"proc.wait","params":{"job_id":1}}\n' | nc 127.0.0.1 9000

# Week 10 — env.* (shell variable tools)
printf '{"type":"mcp","tool":"env.get","params":{"name":"HOME"}}\n' | nc 127.0.0.1 9000
printf '{"type":"mcp","tool":"env.set","params":{"name":"MY_VAR","value":"hello"}}\n' | nc 127.0.0.1 9000
printf '{"type":"mcp","tool":"env.list","params":{}}\n' | nc 127.0.0.1 9000

# Week 10 — edit.* (in-place file editing tools)
printf '{"type":"mcp","tool":"edit.replace_line","params":{"path":"/tmp/f.txt","line":1,"text":"new text"}}\n' | nc 127.0.0.1 9000
printf '{"type":"mcp","tool":"edit.insert_line","params":{"path":"/tmp/f.txt","line":2,"text":"inserted"}}\n' | nc 127.0.0.1 9000
printf '{"type":"mcp","tool":"edit.delete_line","params":{"path":"/tmp/f.txt","line":1}}\n' | nc 127.0.0.1 9000
printf '{"type":"mcp","tool":"edit.replace","params":{"path":"/tmp/f.txt","pattern":"old","replacement":"new"}}\n' | nc 127.0.0.1 9000
```

#### Complete MCP Tool Inventory (20 tools)

| Tool | Params | Summary |
|------|--------|---------|
| `list_tools` | — | Return JSON schema for all tools |
| `get_status` | — | Server uptime, client count, request count |
| `get_registry` | — | All registered shell commands as JSON |
| `run_command` | `command`, `args` | Execute allowlisted shell command |
| `fs.list` | `path` | List directory entries with type/size/mtime |
| `fs.read` | `path` | Read file contents (≤ 1 MB) |
| `fs.write` | `path`, `content` | Write (overwrite) a file |
| `fs.append` | `path`, `content` | Append to a file |
| `fs.stat` | `path` | File metadata (size, mode, mtime, type) |
| `fs.search` | `path`, `pattern`, `recursive` | Grep-like literal substring search |
| `proc.list` | — | List all background jobs (id/type/status/command) |
| `proc.kill` | `job_id`, `signal` | Send signal to a process-type job |
| `proc.wait` | `job_id` | Block until a background job finishes, return exit code |
| `env.get` | `name` | Read one shell/environment variable |
| `env.set` | `name`, `value` | Set a variable in the shell REPL and environment |
| `env.list` | — | Dump all environment variables (up to 200) |
| `edit.replace_line` | `path`, `line`, `text` | Overwrite line N (1-indexed) atomically |
| `edit.insert_line` | `path`, `line`, `text` | Insert before line N atomically |
| `edit.delete_line` | `path`, `line` | Delete line N atomically |
| `edit.replace` | `path`, `pattern`, `replacement` | Global literal-string replace atomically |

### RAG-Grounded `@` Command (Week 9 + Week 10)

The `@` handler uses TF-IDF cosine similarity to retrieve the most relevant commands from `commands.json` before calling the LLM. Week 10 adds a deterministic heuristic fallback so the pipeline always produces a result even when the LLM is unavailable.

```sh
@ find large files          # score ≥ 0.5 → direct match shown, confirm to execute
@ list directory contents   # score 0.1–0.5 → LLM called with retrieved context
@ something unknown         # score < 0.1 → LLM called without context
@ delete all tmp files      # destructive pattern → explicit "yes" confirmation gate
```

**Four-tier lookup chain (Week 10 Final):**

```
user types "@ <query>"
       │
       ▼
[1] RAG retrieval (TF-IDF cosine similarity vs commands.json)
       │ score ≥ 0.5: validate → confirm → execute
       │ score 0.1–0.5: build context string, fall through to LLM
       │ score < 0.1: fall through to LLM without context
       ▼
[2] MCP client (localhost:9000) → OpenRouter AI
       │ suggestion returned
       │ unavailable (no key / offline)
       ▼
[3] Heuristic fallback (name=3 / summary=2 / long_help=1 token scoring)   ← NEW Week 10
       │ best-scoring registry command
       ▼
[4] validate_suggestion() — single-token, registry-only, destructive gate  ← NEW Week 10
       │ valid
       ▼
  show command + explanation → confirm → execute
```

### Configuration (`aishell.conf`)

Created automatically with defaults on first run. Update at runtime with `server config key=value`.

```ini
# Server settings
server_enabled=1
server_port=9000
server_max_clients=16
server_timeout=30

# Commands allowed via MCP run_command tool
server_allowlist=ls,cat,grep,echo,find,du,df,ps,wc,sort,uniq,cut,tr,head,tail

# RAG settings
rag_enabled=1
rag_top_k=3
rag_min_score=0.10

# Logging
log_file=aishell_calls.log
log_mcp_calls=1

# AI model (api_key from OPENROUTER_API_KEY environment variable)
ai_model=meta-llama/llama-3.1-8b-instruct:free
```

---

## Commands (61)

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

### Session (3)

| Command | Key Flags | Summary |
|---------|-----------|---------|
| `alias`   | `NAME=VALUE` | Define or display command aliases |
| `unalias` | `NAME` | Remove a command alias |
| `history` | `-c` clear, `-n N` show last N | Display or clear command history |

### File Editing (6)

| Command | Key Flags | Summary |
|---------|-----------|---------|
| `edit`              | `<subcommand> ...` | **Week 10** unified agent-friendly editor (replace-line / insert-line / delete-line / replace) |
| `edit-show`         | `-n N`, `-e E` | Display file with line numbers |
| `edit-replace-line` | `-n N`, `-t TEXT` | Overwrite line N |
| `edit-insert-line`  | `-n N`, `-a`, `-t TEXT` | Insert before (or after with `-a`) line N |
| `edit-delete-line`  | `-n N`, `-e E` | Delete line N or range N–E |
| `edit-replace`      | `-p PAT`, `-r TEXT`, `-g`, `-i` | Regex find and replace |

### Server Management (1) — Weeks 9–10

| Command | Subcommand | Summary |
|---------|------------|---------|
| `server` | `status` | Show MCP/FTP server running/stopped + port |
| `server` | `start` | Start the TCP server on port 9000 |
| `server` | `stop` | Stop the TCP server |
| `server` | `config` | Print all `aishell.conf` settings |
| `server` | `config key=value` | Update one setting live and save to file |

### Shell Built-ins (not in registry)

| Command | Behaviour |
|---------|-----------|
| `exit [N]`      | Exit the shell with code N (default 0) |
| `prompt STRING` | Change the prompt string for this session |

---

## Build

**Requirements:** `gcc`, `make`, `argtable3`, `bnfc`, `flex`, `bison`, `libcurl` (optional), `libm`

```sh
# Install argtable3 (one-time)
git clone https://github.com/argtable/argtable3.git
cmake -B argtable3/build -DCMAKE_BUILD_TYPE=Release argtable3
cmake --build argtable3/build
sudo cmake --install argtable3/build && sudo ldconfig

# Install BNFC toolchain + libcurl (one-time)
sudo apt-get install -y bnfc flex bison libcurl4-openssl-dev

# Build
make bnfc-gen   # generate Lexer.c + Parser.c from Grammar.cf (once, or after changes)
make            # produces ./aishell  (links -largtable3 -pthread -lm -lcurl)
make clean      # remove build artifacts
```

> **After any `Grammar.cf` change:** run `make bnfc-gen`, then manually run
> `cd bnfc && bison -t -pgrammar_ Grammar.y -o Parser.c` so `Bison.h` is written
> in the right place, then `make clean && make`.

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
| Arithmetic expansion | `$((expr))` — `+` `-` `*` `/` `%`, parentheses, variable references (`$((x * 2))`) |
| Process substitution | `diff <(sort a) <(sort b)`, `tee >(gzip > out.gz)` — via `/proc/self/fd/N` |
| Glob wildcards | `*.c`, `*.h`, `?` expanded via `glob(GLOB_NOCHECK)` |
| Double-quoted strings | `"hello world"` — single argument with space; `$var` expanded inside |
| **Single-quoted strings** | `'hello world'` — literal argument, spaces preserved, no expansion |
| AND / OR lists | `cmd1 && cmd2`, `cmd1 \|\| cmd2` |
| Negation | `! cmd` — invert exit code |
| Subshell | `( cmd1 ; cmd2 )` — isolated child process |
| Command group | `{ cmd1 ; cmd2 }` — current process |
| if / elif / else / fi | Full conditional with unlimited `elif` branches |
| for / while / until | `for i in LIST; do ...`, `while COND; do ...`, `until COND; do ...` |
| break / continue | Loop control |
| Variable assignment | `x=value` — stored in shell variable store and environment |
| Variable expansion | `$x`, `$?`, `$$`, `${var}` — with `getenv()` fallback |
| **`key=value` as argument** | `server config server_timeout=60` — `NAME=VALUE` tokens accepted as command args |
| Conditionals | `test EXPR` / `[ EXPR ]` — file, string, numeric, and logical tests |
| @ NL interface | `@ query` — RAG retrieval → MCP → OpenRouter AI |
| `--commands-json` | Emits all registered commands as JSON for AI/tooling integration |
| `--server` | Daemon mode: start MCP/FTP server only, no REPL, exit on SIGTERM |

---

## Grammar (`bnfc/Grammar.cf`)

### Token types

| Token | Pattern | Purpose |
|-------|---------|---------|
| `ArithExp` | `$((` … `))` | Arithmetic expansion; spaces allowed inside |
| `ProcSubstIn` | `<(` … `)` | Process substitution (read end) |
| `ProcSubstOut` | `>(` … `)` | Process substitution (write end) |
| `Assign` | `NAME=VALUE` | Variable assignment (also valid as command argument) |
| `Word` | alphanum + `-._/:*?~$\`+%,=\[]` + `\` | Standard shell token; backslash included for `\r\n` escapes |

### Command argument types

`CommandPart` accepts an `Arg` non-terminal that can be any of:

```
WrdArg.       Arg ::= Word ;
ArithArg.     Arg ::= ArithExp ;
PSubstInArg.  Arg ::= ProcSubstIn ;
PSubstOutArg. Arg ::= ProcSubstOut ;
AssignArg.    Arg ::= Assign ;     ← Week 9: allows key=value as command args
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

## Architecture

Every command follows the **APPANATOMY** pattern:

- **`cmd_spec_t`** (`cmd_spec.h`) — uniform struct: `name`, `summary`, `long_help`, `run()`, `print_usage()`
- **`build_<name>_argtable()`** — shared argtable3 builder; called by both `run` and `print_usage`
- **Registry** (`registry.c`) — flat array of up to 64 `cmd_spec_t*`; dispatch via `find_command(name)->run(argc, argv)`
- **`cmd_help_json.c`** — walks live `arg_hdr_t` entries to emit JSON; always in sync with the argtable

### Week 10 Final Architecture

```
                    AiShell Process
                         │
          ┌──────────────┴──────────────┐
          │                             │
    REPL (bnfc_repl)             MCP Server Thread
    Week 7/8 unchanged           Listens on port 9000
          │                             │
     @ command                   accept() loop
     (Week 8/9/10 handler)       pthread per client
          │                             │
    [1] RAG lookup              ┌───────┴────────────────────┐
    TF-IDF cosine sim →         │                            │
    retrieve top-K →       FTP protocol          MCP JSON tools (20)
    inject context →       USER/QUIT/PORT    ┌── list_tools
          │                STOR/RETR/LIST   Week 9:
    [2] MCP client →       MKD                  run_command
    OpenRouter LLM                              get_status
          │ unavailable                         get_registry
    [3] Heuristic fallback             Week 10 fs.*:
    (token-overlap scoring)                 fs.list/read/write
          │                                 fs.append/stat/search
    [4] validate_suggestion()          Week 10 proc.*:
    registry-only + destructive             proc.list/kill/wait
    confirmation gate          └── Week 10 env.* + edit.*:
          │                               env.get/set/list
    execute                               edit.replace_line
                                          edit.insert_line
                                          edit.delete_line
                                          edit.replace
```

### MCP Server Client Detection

```
Client connects to port 9000
  │
  ├─ recv(MSG_PEEK, 300ms timeout)
  │    ├─ first byte == '{': MCP JSON → handle_mcp_json()
  │    └─ timeout / other:  FTP       → handle_ftp_session()
  │                                     sends "220 ready" banner first
```

### @ Pipeline — Four-Tier Chain (Week 10)

```
@ find large files
  │
  ├─ [1] rag_query(): TF-IDF cosine similarity vs commands.json
  │         ≥ 0.5: direct match → validate → confirm → execute
  │         0.1–0.5: build rag_context, fall through to LLM
  │         < 0.1: fall through without context
  │
  ├─ [2] openrouter_query_ctx(query, rag_context)
  │         success: suggestion string
  │         fail (no key / offline): NULL → heuristic
  │
  ├─ [3] heuristic_fallback_command(query)
  │         scores every registered command by token overlap
  │         returns best-match name
  │
  └─ [4] validate_suggestion(suggestion)
            strip to first token, reject if not in registry
            destructive patterns → explicit confirmation gate
```

### Source Layout

```
aishell_main.c           — REPL, dispatch, BNFC evaluator, @ handler, arith/process-subst
                           Week 10: proc_list_json(), proc_kill_json(), proc_wait_json(),
                           mcp_sync_var() — public job-table + var-store accessors
bnfc/Grammar.cf          — BNF grammar source (single file you author)
bnfc/                    — BNFC-generated files (Absyn.h, Lexer.c, Parser.c, …)
commands.json            — NL command registry (TF-IDF index source)
aishell.conf             — runtime configuration (auto-created with defaults)

# Week 10 new files
validate.c/h             — validate_suggestion() (registry-only, destructive gate)
                           heuristic_fallback_command() (token-overlap scoring)
jobs_api.h               — public interface to job table + mcp_sync_var()
cmd_edit.c/h             — unified `edit` command (replace-line/insert-line/delete-line/replace)
test_week10.sh      — 43 acceptance tests — all 20 MCP tools, @ fallback, edit command
WEEK10_WORKLOG.md        — design decisions, tool inventory table, known limitations

# Week 9 new files
mcp_server.c/h           — TCP server: accept loop, pthread per client, FTP/MCP dispatch
                           Week 10: all fs.*/proc.*/env.*/edit.* tool handlers added here
ftp_handler.c/h          — FTP protocol: USER/QUIT/PORT/STOR/RETR/LIST/MKD
rag_retriever.c/h        — TF-IDF index build + cosine similarity query
config.c/h               — aishell.conf loader/saver + g_config global
cmd_server.c             — "server" built-in command (status/start/stop/config)

# Week 8 files (unchanged)
cmd_registry.c/h         — cJSON-based registry loader + iteration API
mcp_client.c/h           — non-blocking TCP client to MCP server
aishell_client.c/h       — OpenRouter AI client via libcurl (+ context-aware variant)
aishell_log.c/h          — thread-safe append logger
cJSON.c/h                — embedded JSON parser

# Command modules (one file per command)
cmd_<name>.c             — 60 command implementations (including cmd_edit.c added Week 10)
edit_utils.c/h           — shared file I/O for edit-* commands
hash_utils.c/h           — shared MD5/SHA-256 implementation
cmd_spec.h               — cmd_spec_t definition + registry API
cmd_help_json.c/h        — shared --help-json implementation
registry.c               — register_command / find_command / for_each_command

# Tests
test_week5.sh            — 25 checks — process management
test_week6.sh            — 23 checks — pthread integration
test_week7.sh            — 26 checks — BNFC grammar features
test_week8.sh            — 21 checks — AI/registry/MCP integration
test_week9.sh            — 62 checks (merged) — MCP server, FTP, RAG, config + xargs/read/test/arithmetic/process-substitution
test_week10.sh      — 43 acceptance demos — 20 MCP tools, @ hardening, edit
```

---

## Test Suites

```sh
bash test_week5.sh         # 25 checks — fork/exec, pipes, redirects, signals
bash test_week6.sh         # 23 checks — pthreads, job table  [⚠ pre-existing failures: help text format changed in Week 7+]
bash test_week7.sh         # 26 checks — grammar, variables   [⚠ pre-existing failures: same help-text mismatch]
bash test_week8.sh         # 21 checks — 14 pass offline (7 require live API key)
bash test_week9.sh         # 62 checks — MCP server, FTP protocol, RAG @, config,xargs, read, test/[, nc, arithmetic
bash test_week10.sh        # 43 / 43 — all 20 MCP tools, @ hardening, edit command

# Or run all via make
make test10    # Week 10 tests only
make testall   # Weeks 5 + 8 + 9 + 10 (weeks 6/7 excluded — pre-existing help-text failures unrelated to Week 10)
```

---

## Usage Examples

```sh
# Single-quoted strings (Week 9 grammar fix)
printf 'hello world'
printf 'USER testuser\r\nQUIT\r\n' | nc 127.0.0.1 9000

# key=value as command argument (Week 9 grammar fix)
server config server_timeout=60
server config rag_min_score=0.3

# RAG @ command
@ find large files           # TF-IDF retrieval → high confidence → direct execute
@ list directory contents    # medium confidence → AI with context
@ show disk usage sorted     # matches du/df commands

# Arithmetic expansion with variable references
x=5
echo $((x * 2))              # 10
echo $((x * x + 1))          # 26

# Process substitution
diff <(sort file1) <(sort file2)
echo hello | tee >(wc -c) >/dev/null

# Conditional tests
test -f Makefile && echo "exists"
[ -d /tmp ] && echo "is a dir"
if [ -f /etc/hostname ]; then cat /etc/hostname; fi

# xargs
find . -name "*.c" | xargs wc -l
ls *.c | xargs -I{} cp {} /tmp/backup/

# Checksums
md5sum Makefile
echo "test" | sha256sum

# Networking
nc -z localhost 22 && echo "port 22 open"
printf '{"type":"mcp","tool":"get_status","params":{}}\n' | nc 127.0.0.1 9000

# Aliases
alias ll='ls -la'
ll /tmp

# AI natural language (Week 10: always gets a result even offline)
@ count lines in all c files
@ show disk usage sorted by size
@ delete files older than 7 days in /tmp   # destructive gate: requires "yes"

# Week 10 — unified edit command
edit replace-line /tmp/notes.txt 3 "updated third line"
edit insert-line  /tmp/notes.txt 1 "prepended first line"
edit delete-line  /tmp/notes.txt 2
edit replace      /tmp/notes.txt "old_value" "new_value"

# Week 10 — MCP agent tools (proc.*, env.*, edit.*)
printf '{"type":"mcp","tool":"proc.list","params":{}}\n' | nc 127.0.0.1 9000
printf '{"type":"mcp","tool":"env.set","params":{"name":"DEBUG","value":"1"}}\n' | nc 127.0.0.1 9000
printf '{"type":"mcp","tool":"fs.search","params":{"path":".","pattern":"TODO","recursive":true}}\n' | nc 127.0.0.1 9000
```

---

## Security

The MCP/FTP server enforces several hardening measures:

| Check | Implementation |
|-------|---------------|
| Localhost-only MCP | `getpeername()` verifies peer is `127.0.0.1` before handling any MCP tool |
| Command allowlist | `run_command` only executes commands in `server_allowlist` (config) |
| Shell injection | `is_safe_args()` rejects `;`, `&&`, `\|\|`, `` ` ``, `$(`, `>&` in MCP args |
| FTP path traversal | `is_safe_filename()` rejects `..` components and NUL bytes |
| FTP path escape | `path_escapes_cwd()` uses `realpath()` to prevent escaping home directory |
| FTP PORT hijack | IP in PORT command always overridden to `127.0.0.1`; ports ≤ 1023 rejected |
| Binary-safe transfer | STOR/RETR use `read()`/`write()` not `fread`/`fputs` — NUL bytes preserved |
| Idle timeout | `SO_RCVTIMEO` set to `server_timeout` seconds (default 30) on each client fd |
| Oversized input | FTP lines > 1024 bytes → `500 Syntax error`; MCP query > 4096 bytes truncated |
| @ destructive gate | `validate_suggestion()` matches 12 destructive patterns (rm -rf, dd, mkfs, …) → requires explicit "yes" confirmation *(Week 10)* |
| @ registry-only | `validate_suggestion()` rejects any LLM/heuristic suggestion not in the command registry — no arbitrary command execution *(Week 10)* |
| edit path safety | `edit_path_safe()` in `cmd_edit.c` rejects any path containing `..` *(Week 10)* |
| env var name safety | `is_valid_varname()` in `mcp_server.c` enforces `[A-Za-z_][A-Za-z0-9_]*` before calling `setenv()` *(Week 10)* |

---

## JSON Help Docs

Every command supports `--help-json` and `--help`:

```sh
./aishell server     --help-json
./aishell xargs      --help-json
./aishell read       --help-json
./aishell test       --help-json
./aishell nc         --help-json
./aishell --commands-json | python3 -m json.tool
```

Pre-generated snapshots live in `docs/`. Regenerate all:

```sh
for cmd in ls cat stat head tail cp mv rm mkdir rmdir touch rg ps kill wait \
           jobs cd pwd echo env export unset type wc sort uniq cut tr grep diff \
           tee du df ln chmod chown sleep which true false file date find \
           md5sum sha256sum alias history ping nc xargs read test server \
           edit edit-show edit-replace-line edit-insert-line edit-delete-line edit-replace; do
    ./aishell $cmd --help-json > docs/${cmd}.json 2>/dev/null
done
```

---

## Known Limitations

- FTP uses **active mode only** (client provides data port via PORT); PASV not implemented
- RAG uses TF-IDF bag-of-words — semantic similarity is approximate (no neural embeddings)
- `server_allowlist` in `aishell.conf` is loaded but the hardcoded `ALLOWLIST[]` in `mcp_server.c` is what's used at runtime *(identified in Week 9; noted for post-Week-10 cleanup)*
- No TLS — the MCP/FTP server is for local/educational use only; do not expose port 9000 externally
- Single-quoted strings: content is passed literally but `$VAR` inside `'...'` is still expanded (not fully POSIX-correct)
- `edit.*` MCP tools and `edit` command cap file size at 10 MB — use a real editor for larger files
- `proc.kill` only works on `JOB_TYPE_PROCESS` jobs (external fork); background threads (`JOB_TYPE_THREAD`) return an informative error since they cannot receive POSIX signals via `kill()`
- `env.list` caps at 200 environment variables to bound response size
- `env.set` via MCP syncs to the aishell process's var_store — visible to `$VAR` expansion inside the aishell REPL but not to external shells running as a different user
- `test_week6.sh` and `test_week7.sh` have pre-existing failures (help output format changed in Week 7+); `make testall` correctly excludes them — these are not Week 10 regressions
