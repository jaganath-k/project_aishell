# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is the week3 iteration of the aishell project, building on week2 ([aishell-week2](https://github.com/jaganath-k/aishell-week2)). It extends the same command-registry architecture with argtable3-based option parsing, an expanded command set, and a BusyBox-style multi-call shell binary (`aishell`).

### Commands (59 total)

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
| `wc`     | `cmd_wc.c`     | count lines, words, and bytes |
| `sort`   | `cmd_sort.c`   | sort lines of text |
| `uniq`   | `cmd_uniq.c`   | report or omit repeated lines |
| `cut`    | `cmd_cut.c`    | remove sections from each line |
| `tr`     | `cmd_tr.c`     | translate or delete characters |
| `grep`   | `cmd_grep.c`   | print lines matching a pattern |
| `diff`   | `cmd_diff.c`   | compare files line by line |
| `tee`    | `cmd_tee.c`    | read stdin, write to stdout and files |
| `du`     | `cmd_du.c`     | estimate file space usage |
| `df`     | `cmd_df.c`     | report file system disk space usage |
| `ln`     | `cmd_ln.c`     | create hard or symbolic links |
| `chmod`  | `cmd_chmod.c`  | change file permissions |
| `chown`  | `cmd_chown.c`  | change file owner and group |
| `sleep`  | `cmd_sleep.c`  | suspend execution for a time interval |
| `which`  | `cmd_which.c`  | locate a command |
| `true`   | `cmd_true_false.c` | return true exit status |
| `false`  | `cmd_true_false.c` | return false exit status |
| `file`   | `cmd_file.c`   | determine file type |
| `date`   | `cmd_date.c`   | print or set the system date and time |
| `find`   | `cmd_find.c`   | search for files in a directory hierarchy |
| `md5sum` | `cmd_md5sum.c` | compute MD5 checksum |
| `sha256sum` | `cmd_sha256sum.c` | compute SHA-256 checksum |
| `alias`  | `cmd_alias.c`  | define or display command aliases |
| `unalias`| `cmd_alias.c`  | remove command aliases |
| `history`| `cmd_history.c`| display command history |
| `ping`   | `cmd_ping.c`   | send ICMP echo requests |
| `nc`     | `cmd_nc.c`     | TCP client/server (netcat-like) |
| `xargs`  | `cmd_xargs.c`  | build and execute commands from stdin |
| `read`   | `cmd_read.c`   | read a line from stdin into a variable |
| `test`   | `cmd_test.c`   | evaluate a conditional expression |
| `[`      | `cmd_test.c`   | evaluate a conditional expression (alias for test) |
| `edit-replace-line` | `cmd_edit_replace_line.c` | replace a line in a file by number |
| `edit-insert-line`  | `cmd_edit_insert_line.c`  | insert a line into a file |
| `edit-delete-line`  | `cmd_edit_delete_line.c`  | delete a line from a file |
| `edit-replace`      | `cmd_edit_replace.c`      | replace text in a file |
| `edit-show`         | `cmd_edit_show.c`         | display a file with line numbers |

## Build Commands

```sh
make          # compile all source files into a single ./aishell binary
make clean    # remove build artifacts
make bnfc-gen # regenerate lexer/parser from bnfc/Grammar.cf (run after grammar changes)
```

Build flags: `-Wall -Wextra -std=c11 -pthread`, links against `-largtable3 -lcurl`.

argtable3 is installed at `/usr/local/include/argtable3.h`; link with `-largtable3`.

After any change to `bnfc/Grammar.cf`, run:
```sh
make bnfc-gen   # regenerates Grammar.l, Grammar.y, Absyn.h, etc.
# bison runs from within bnfc/ to ensure Bison.h is written in the right place
make clean && make
```

## Architecture

### Command Module Pattern (APPANATOMY)

Every command follows the anatomy defined in `APPANATOMY.md`:

1. **`cmd_spec_t`** (`cmd_spec.h`) — single struct: `name`, `summary`, `long_help`, `run(argc,argv)`, `print_usage(FILE*)`.
2. **`argtable3` for all CLI parsing** — each command defines a `build_<name>_argtable()` helper with pointer out-parameters for every arg plus `void ***argtable_out`. Both `run` and `print_usage` call this builder to get fresh local pointers — no module-level statics.
3. **Registry** (`registry.c`) — flat static array of up to 64 `cmd_spec_t*`. Commands register via `register_<name>_command()` → `register_command(&cmd_<name>_spec)`. Shell dispatches via `find_command(name)->run(argc, argv)`.
4. **Single binary** — all source files compile into one `aishell` binary. No separate `.o` targets or standalone command binaries.
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

`aishell_main.c` implements a BusyBox-style multi-call binary and interactive shell powered by a BNFC-generated lexer/parser.

### Multi-call dispatch

When invoked under a command name other than `aishell`, it dispatches via `basename(argv[0])` through the registry:

```sh
cp aishell /tmp/ls && /tmp/ls /tmp   # dispatches to ls_run()
```

### Interactive REPL

When invoked as `aishell`, it runs an interactive shell with a BNFC-generated recursive-descent parser producing a full AST, then walking the AST to execute commands.

#### Preprocessing pipeline (before BNFC parse)

```
getline (EINTR-safe)
  → preprocess_arith()      evaluate $((expr)) — pure-constant ones eagerly
  → preprocess_cmd_subst()  run $(cmd) via popen, substitute stdout
  → preprocess_quotes()     replace spaces inside "..." with ` placeholder
  → BNFC parse              Grammar.cf → full AST
  → AST executor            eval_job / eval_cmdline / eval_cmdpart
```

#### REPL features

| Feature | Detail |
|---------|--------|
| Signal masking | SIGINT, SIGQUIT, SIGTSTP blocked — shell survives Ctrl-C/Z/\ |
| EINTR retry | `getline` retried when interrupted by a signal |
| Colored prompt | Yellow ANSI (`\033[0;33m`) on terminals; plain text when piped |
| Default prompt | `% ` (reconfigurable with `prompt STRING`) |
| Pipelines | `cmd1 \| cmd2 \| cmd3` multi-stage pipelines |
| Sequential | `cmd1 ; cmd2` or `cmd1;cmd2` (spaces optional) |
| AND / OR lists | `cmd1 && cmd2`, `cmd1 \|\| cmd2` |
| Output redirection | `cmd > file`, `cmd >> file` (append), `cmd &> file` |
| Input redirection | `cmd < file` |
| Stderr redirect | `cmd 2> file`, `cmd 2>> file` |
| Background | `cmd &` |
| Glob wildcards | `*.c`, `*.h`, `?` expanded via `glob(GLOB_NOCHECK)` |
| Variable expansion | `$VAR`, `${VAR}`, `$?`, `$$` |
| Arithmetic expansion | `$((expr))` with `+ - * / %` and variable references |
| Command substitution | `$(cmd)` — output substituted as argument |
| Process substitution | `<(cmd)` and `>(cmd)` — pipe via `/proc/self/fd/N` |
| if/elif/else/fi | Full conditional with `[ expr ]` or `test` |
| for/do/done | `for VAR in LIST; do CMDS; done` |
| while/until | `while COND; do CMDS; done`, `until COND; do CMDS; done` |
| break / continue | Loop control |
| Subshell | `(cmds)` — run in child process |
| Command group | `{cmds}` — run in current shell |
| Negation | `! cmd` — invert exit status |
| time | `time cmd` — report execution time |

#### Shell built-ins (handled before registry lookup)

| Command | Behaviour |
|---------|-----------|
| `exit [N]` | Exit with code N (default 0) |
| `prompt STRING` | Set prompt to `STRING ` for this session |

`cd`, `pwd`, `echo` are **registered commands** (full APPANATOMY modules), not special-cased.

### `cmd_t` struct

```c
typedef struct {
    int    first, last;
    char  *sep;
    char **argv;
    int    argc;
    char  *stdin_file;
    char  *stdout_file;
    int    stdout_append;
    char  *stderr_file;
    int    stderr_append;
    int    stderr_both;
    int    background;
    int    pipe_next;
    pid_t  ps_pids[8];   /* process-substitution child PIDs */
    int    ps_fds[8];    /* pipe ends to close after exec */
    int    ps_count;
} cmd_t;
```

### BNFC Grammar (`bnfc/Grammar.cf`)

Key tokens:
- `Word` — standard shell token; includes `$`, `[`, `]`, `=`, `+`, `%`, `,`
- `Assign` — `NAME=VALUE` assignments (parsed before Word)
- `ArithExp` — `$((expr))` arithmetic expansion (spaces allowed inside)
- `ProcSubstIn` — `<(cmd)` process substitution
- `ProcSubstOut` — `>(cmd)` process substitution

`CommandPart` uses an `Arg` non-terminal that accepts `Word`, `ArithExp`, `ProcSubstIn`, or `ProcSubstOut` as arguments.

## JSON Help Docs

Pre-generated snapshots live in `docs/`. Regenerate with:

```sh
for cmd in ls cat stat head tail cp mv rm mkdir rmdir touch rg ps kill wait jobs \
           cd pwd echo env export unset type wc sort uniq cut tr grep diff tee du df \
           ln chmod chown sleep which true false file date find md5sum sha256sum \
           alias history ping nc xargs read test; do
    ./aishell $cmd --help-json > docs/${cmd}.json 2>/dev/null
done
```

## Key Conventions

- New command: create `cmd_<name>.c`, add to `SRCS` in `Makefile`, call `register_<name>_command()` in `aishell_main.c:register_all_commands()`, add `extern void register_<name>_command(void);` declaration at the top of `aishell_main.c`.
- Always include `--help-json` in every new command's argtable; use `print_help_json(&cmd_<name>_spec, argtable, stdout)` from `cmd_help_json.h`.
- `print_usage` and `run` share argtable definitions via `build_<name>_argtable()` — never duplicate option definitions.
- Forward-declare `extern cmd_spec_t cmd_<name>_spec;` before `run()` so `print_help_json` can reference it.
- `cmd_spec_t.summary` and `long_help` feed the `help` built-in and `--help-json` output; keep them accurate.
- `MAX_COMMANDS` cap is 64 in `registry.c`; increase if needed.
- Glob expansion in the shell uses `GLOB_NOCHECK` — unmatched patterns are passed through unchanged.
- Grammar changes: edit `bnfc/Grammar.cf`, run `make bnfc-gen`, then run bison from within `bnfc/` so `Bison.h` lands in the right place (`cd bnfc && bison -t -pgrammar_ Grammar.y -o Parser.c`), then `make clean && make`.
- `arith_eval()` and `expand_vars()` in `aishell_main.c` handle `$((expr))` at runtime with variable lookup via `var_get()`.
- Process substitution children are tracked in `cmd_t.ps_pids[]` / `ps_fds[]` and cleaned up by `free_bnfc_cmds()` after the outer command finishes.
