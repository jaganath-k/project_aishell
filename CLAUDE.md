# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is the week3 iteration of the aishell project, building on week2 ([aishell-week2](https://github.com/jaganath-k/aishell-week2)). It extends the same command-registry architecture with argtable3-based option parsing, an expanded command set, and a BusyBox-style multi-call shell binary (`aishell`).

### Commands (23 total)

| Command | Source | Summary |
|---------|--------|---------|
| `ls`     | `cmd_ls.c`     | list directory contents |
| `cat`    | `cmd_cat.c`    | concatenate files and print on stdout |
| `stat`   | `cmd_stat.c`   | display file or file system status |
| `head`   | `cmd_head.c`   | output the first part of files |
| `tail`   | `cmd_tail.c`   | output the last part of files |
| `cp`     | `cmd_cp.c`     | copy files |
| `mv`     | `cmd_mv.c`     | move/rename files |
| `rm`     | `cmd_rm.c`     | remove files |
| `mkdir`  | `cmd_mkdir.c`  | make directories |
| `rmdir`  | `cmd_rmdir.c`  | remove empty directories |
| `touch`  | `cmd_touch.c`  | change file timestamps / create files |
| `rg`     | `cmd_rg.c`     | search files for a pattern (regex-aware) |
| `ps`     | `cmd_ps.c`     | report process status |
| `kill`   | `cmd_kill.c`   | send a signal to a process |
| `wait`   | `cmd_wait.c`   | wait for process completion |
| `jobs`   | `cmd_jobs.c`   | list processes in current session |
| `cd`     | `cmd_cd.c`     | change the working directory |
| `pwd`    | `cmd_pwd.c`    | print name of current/working directory |
| `echo`   | `cmd_echo.c`   | display a line of text |
| `env`    | `cmd_env.c`    | print environment or set variables for display |
| `export` | `cmd_export.c` | set or display environment variables |
| `unset`  | `cmd_unset.c`  | remove variables from the environment |
| `type`   | `cmd_type.c`   | show how each name would be interpreted by the shell |

## Build Commands

```sh
make          # compile all 26 source files into a single ./aishell binary
make clean    # remove build artifacts
```

Build flags: `-Wall -Wextra -std=c11`, links against `-largtable3`.

argtable3 is installed at `/usr/local/include/argtable3.h`; link with `-largtable3`.

## Architecture

### Command Module Pattern (APPANATOMY)

Every command follows the anatomy defined in `APPANATOMY.md`:

1. **`cmd_spec_t`** (`cmd_spec.h`) — single struct: `name`, `summary`, `long_help`, `run(argc,argv)`, `print_usage(FILE*)`.
2. **`argtable3` for all CLI parsing** — each command defines a `build_<name>_argtable()` helper with pointer out-parameters for every arg plus `void ***argtable_out`. Both `run` and `print_usage` call this builder to get fresh local pointers — no module-level statics.
3. **Registry** (`registry.c`) — flat static array of up to 32 `cmd_spec_t*`. Commands register via `register_<name>_command()` → `register_command(&cmd_<name>_spec)`. Shell dispatches via `find_command(name)->run(argc, argv)`.
4. **Single binary** — all 26 source files compile into one `aishell` binary. No separate `.o` targets or standalone command binaries.
5. **`--help-json`** — every command supports `--help-json` via the shared `print_help_json()` in `cmd_help_json.c`. It walks live `arg_hdr_t` entries so JSON stays in sync with the argtable automatically.

### `build_<name>_argtable()` required pattern

```c
static void build_ls_argtable(arg_lit_t  **help,
                               arg_lit_t  **help_json,
                               arg_lit_t  **all,
                               arg_file_t **files,
                               arg_end_t  **end,
                               void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json", "print machine-readable help as JSON");
    /* ... other args ... */
    *end       = arg_end(20);

    static void *argtable[N];   /* N = number of args + 1 for NULL sentinel */
    argtable[0] = *help;
    /* ... */
    argtable[N-1] = NULL;
    *argtable_out = argtable;
}
```

Free the argtable **before** calling `print_usage`; free **after** calling `print_help_json` (it needs the live table):

```c
if (help->count > 0) {
    arg_freetable(argtable, N-1);
    ls_print_usage(stdout);
    return 0;
}
if (help_json->count > 0) {
    print_help_json(&cmd_ls_spec, argtable, stdout);
    arg_freetable(argtable, N-1);
    return 0;
}
if (nerrors > 0) {
    arg_print_errors(stdout, end, "ls");
    arg_freetable(argtable, N-1);
    ls_print_usage(stdout);
    return 1;
}
```

## BusyBox Multi-call Shell (`aishell`)

`aishell_main.c` implements a BusyBox-style multi-call binary and interactive shell.

### Multi-call dispatch

When invoked under a command name other than `aishell`, it dispatches via `basename(argv[0])` through the registry:

```sh
cp aishell /tmp/ls && /tmp/ls /tmp   # dispatches to ls_run()
```

### Interactive REPL

When invoked as `aishell`, it runs an interactive shell with the following pipeline (mirrors the reference Unix_Shell.c architecture):

```
getline (EINTR-safe)
  → preprocess()          pad ; < > with spaces
  → tokenise()            flat token[] array   [Token.c equivalent]
  → separate_commands()   cmd_t[] structs      [Command.c equivalent]
  → execute_command()     registry dispatch    [Builtin.c equivalent]
```

#### REPL features

| Feature | Detail |
|---------|--------|
| Signal masking | SIGINT, SIGQUIT, SIGTSTP blocked — shell survives Ctrl-C/Z/\ |
| EINTR retry | `getline` retried when interrupted by a signal |
| Colored prompt | Yellow ANSI (`\033[0;33m`) on terminals; plain text when piped |
| Default prompt | `% ` (reconfigurable with `prompt STRING`) |
| Sequential execution | `cmd1 ; cmd2` or `cmd1;cmd2` (spaces optional) |
| Output redirection | `cmd > file` or `cmd>file` |
| Input redirection | `cmd < file` or `cmd<file` |
| Glob wildcards | `*.c`, `*.h`, `?` expanded via `glob(GLOB_NOCHECK)` |
| Path-prefix strip | `./ls`, `/usr/bin/ls`, `ls` all resolve to the same registry entry |

#### Shell built-ins (handled before registry lookup)

| Command | Behaviour |
|---------|-----------|
| `exit [N]` | Exit with code N (default 0) |
| `prompt STRING` | Set prompt to `STRING ` for this session |

`cd`, `pwd`, `echo` are **registered commands** (full APPANATOMY modules), not special-cased.

### `cmd_t` struct (Command.c equivalent)

```c
typedef struct {
    int    first;        /* index of first token */
    int    last;         /* index of last token  */
    char  *sep;          /* separator: ";"        */
    char **argv;         /* glob-expanded, heap-alloc'd */
    int    argc;
    char  *stdin_file;   /* filename for < redirection */
    char  *stdout_file;  /* filename for > redirection */
} cmd_t;
```

## JSON Help Docs

Pre-generated snapshots for all 23 commands live in `docs/`. Regenerate with:

```sh
for cmd in ls cat stat head tail cp mv rm mkdir rmdir touch rg ps kill wait jobs \
           cd pwd echo env export unset type; do
    ./aishell $cmd --help-json > docs/${cmd}.json
done
```

## Key Conventions

- New command: create `cmd_<name>.c`, add to `SRCS` in `Makefile`, call `register_<name>_command()` in `aishell_main.c:register_all_commands()`, add `extern void register_<name>_command(void);` declaration at the top of `aishell_main.c`.
- Always include `--help-json` in every new command's argtable; use `print_help_json(&cmd_<name>_spec, argtable, stdout)` from `cmd_help_json.h`.
- `print_usage` and `run` share argtable definitions via `build_<name>_argtable()` — never duplicate option definitions.
- Forward-declare `extern cmd_spec_t cmd_<name>_spec;` before `run()` so `print_help_json` can reference it.
- `cmd_spec_t.summary` and `long_help` feed the `help` built-in and `--help-json` output; keep them accurate.
- `MAX_COMMANDS` cap is 32 in `registry.c`; increase if needed.
- Glob expansion in the shell uses `GLOB_NOCHECK` — unmatched patterns are passed through unchanged.
- `preprocess()` in `aishell_main.c` pads `;`, `<`, `>` with spaces before tokenising so operators work with or without surrounding spaces.
