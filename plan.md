# Refactor Plan — aishell_main.c (A + B + C)

## Context

The original `aishell_main.c` used a single-pass approach: `strtok_r(";")` split
the line into segments, then `run_segment()` tokenised each segment ad-hoc.
This was compared against the reference `Unix_Shell.c` from the assignment PDF
and three gaps were identified and approved for fixing.

---

## Change A — Glob Wildcard Expansion

**Problem:** Shell objective #5 (wildcards) not met.
Typing `echo *.c` passed the literal string `*.c` to the command.

**Solution:** Add `build_cmd_argv()` that runs every token through
`glob(token, GLOB_NOCHECK, NULL, &gr)` before building `argv[]`.

- `GLOB_NOCHECK` — if no file matches, the original pattern is returned
  unchanged (correct POSIX shell behaviour).
- Two-pass approach: count expanded args first, then allocate and fill.
- Skip `<` / `>` operators **and** their following filename token during
  both passes.
- Results are `strdup`'d into a heap-allocated `argv[]`; freed after
  each command via `free_cmd_argv()`.

**Files changed:** `aishell_main.c` — new `build_cmd_argv()` function.

---

## Change B — Colored Prompt + Default `%`

**Problem:** Prompt was plain `aishell> `; reference uses yellow ANSI prompt
and `%` as the default.

**Solution:**
- Change default prompt string from `"aishell> "` to `"% "`.
- Print prompt with `printf("\033[0;33m%s\033[0m", prompt)` when stdout is
  a terminal; fall back to plain `fputs(prompt, stdout)` when piped.
  Guard: `isatty(STDOUT_FILENO)`.

**Files changed:** `aishell_main.c` — `shell_repl()` prompt initialisation
and print statement.

---

## Change C — Two-Phase Token + Command Architecture

**Problem:** The original single-pass `run_segment()` was a structural mismatch
with the reference `Unix_Shell.c` (Token.c / Command.c / Builtin.c layers).
Semicolons without surrounding spaces (`cmd1;cmd2`) also did not split
correctly because `strtok_r(";")` was applied to the whole line before
tokenising, meaning a word like `one;two` became one token.

**Solution:** Replace `run_segment()` with a clean three-phase pipeline:

```
getline (EINTR-safe)
  → preprocess()            pad ; < > with spaces
  → tokenise()              flat token[] array         [mirrors Token.c]
  → separate_commands()     cmd_t[] structs            [mirrors Command.c]
  → execute_command()       registry dispatch          [mirrors Builtin.c]
```

### New `preprocess()` function
Surrounds `;`, `<`, `>` with spaces before tokenising so operators always
become standalone tokens regardless of whether the user typed spaces around
them.  Returns a new heap-allocated string; caller must `free()`.

> Note: The reference PDF acknowledges the same limitation —
> *"It also has to contain a space between each eliminator which could be
> improved."*  `preprocess()` is our fix for this.

### New `cmd_t` struct  (mirrors `Command.h`)
```c
typedef struct {
    int    first;        // index of first token in flat token[]
    int    last;         // index of last token in flat token[]
    char  *sep;          // separator following this command (";")
    char **argv;         // glob-expanded, heap-allocated argument vector
    int    argc;         // number of entries in argv
    char  *stdin_file;   // filename for < redirection (pointer into token[])
    char  *stdout_file;  // filename for > redirection (pointer into token[])
} cmd_t;
```

### Function breakdown

| Function | Mirrors | Responsibility |
|---|---|---|
| `preprocess(line)` | *(not in reference)* | Pad `;` `<` `>` with spaces |
| `tokenise(line, token[])` | `Token.c` | `strtok_r` split into flat array |
| `search_redirection(token[], cmd)` | `searchRedirection()` | Find `<`/`>`, set `stdin_file`/`stdout_file` |
| `build_cmd_argv(token[], cmd)` | `buildCommandArgumentArray()` | Glob-expand tokens → heap argv[] |
| `separate_commands(token[], cmds[])` | `separateCommands()` | Build `cmd_t[]` from token array |
| `free_cmd_argv(cmd)` | *(not in reference)* | Free strdup'd argv strings |
| `execute_command(cmd, prompt, sz)` | `execute_command()` / `Builtin.c` | Builtins + I/O redirect + registry dispatch |

### Memory ownership

```
line buffer (getline)
  └─ processed buffer (preprocess, heap)
       └─ token[] entries point INTO processed
            └─ cmd.stdin_file / cmd.stdout_file point INTO token[]
                 (safe: both used before processed is freed)
  cmd.argv[] — independently strdup'd, freed by free_cmd_argv()
```

**Files changed:** `aishell_main.c` — full rewrite of everything between
`register_all_commands()` and `main()`.

---

## Execution Order

1. Add `#include <glob.h>` to includes.
2. Add `cmd_t` struct definition.
3. Write `preprocess()`.
4. Write `tokenise()` (replaces the `strtok_r(";")` loop).
5. Write `search_redirection()`.
6. Write `build_cmd_argv()` (includes glob expansion — Change A).
7. Write `separate_commands()`.
8. Write `free_cmd_argv()`.
9. Write `execute_command()` (port redirect + registry logic from `run_segment()`).
10. Rewrite `shell_repl()`:
    - Change default prompt to `"% "` and add ANSI color guard (Change B).
    - Replace `strtok_r(";")` loop with `preprocess → tokenise → separate_commands → execute_command` pipeline (Change C).
11. Keep `main()` (BusyBox dispatch) unchanged.
12. Build: `make aishell` — verify zero warnings.

---

## Verification Checklist

| Test | Expected |
|---|---|
| `echo one;echo two;echo three` | Three separate output lines |
| `echo hello;echo world` | Two lines (no spaces around `;`) |
| `echo *.h` | Expands to `cmd_help_json.h cmd_spec.h` |
| `echo *.xyz` | Prints `*.xyz` (no match, passes through) |
| `echo hello > /tmp/t.txt` then `cat /tmp/t.txt` | `hello` |
| `echo hello>/tmp/t.txt` (no spaces) | Same — preprocess fixes this |
| Prompt on startup | `% ` in yellow on terminal |
| `prompt $` then command | Prompt changes to `$ ` |
| `exit 42` | Shell exits; `echo $?` gives `42` |
| `./ls .` inside shell | Works (cmd_basename strips `./`) |
