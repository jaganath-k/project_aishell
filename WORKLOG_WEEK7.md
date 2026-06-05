# AiShell — Week 7 Work Log
## Grammar-Based Parsing (BNF/BNFC)

**Project:** AiShell — BusyBox-style shell in C  
**Week:** 7  
**Primary files:** `bnfc/Grammar.cf`, `aishell_main.c` (`#ifdef USE_BNFC` section)  
**Test suite:** `test_week7.sh` — 26 checks, all passing  
**Demo:** `demo_week7.sh`  

---

## 1. Overview

Week 7 replaces AiShell's hand-rolled tokeniser (`preprocess → tokenise → separate_commands`) with a formal BNF grammar processed by BNFC (BNF Converter). BNFC reads `Grammar.cf` and generates a lexer, an LR parser, and typed AST types. An evaluator layer bridges the typed AST to the existing Week 5/6 execution infrastructure — pipelines, redirection, job table, pthreads — all unchanged.

**Three-layer architecture:**
```
Input line
  → preprocess_arith()    $((expr)) → numeric result
  → preprocess_quotes()   "hello world" → hello`world
  → psInput(line)         BNFC LR parser → typed AST
  → eval_input/eval_job   AST walker
  → execute_command / run_pipeline    (Week 5/6, unchanged)
```

---

## 2. Grammar — `bnfc/Grammar.cf` (30 rules)

### BNFC 2.9.5 Syntax Rules Applied

| Rule | Correct Syntax |
|---|---|
| Entry point | `entrypoints` (not `entry`) |
| Character classes | Double-quoted strings inside `[]`: `["-._/:*?~$`"]` |
| Char literals in alternation | Single-quoted: `'_'`, `'='` |
| List categories | `[Cat]` in rules; `separator` macro generates them |
| flex prefix | `-Pgrammar_` flag required |
| bison prefix | `-pgrammar_` flag required |

### Token Definitions

```
token Word   ((letter | digit | ["-._/:*?~$`"])
              (letter | digit | ["-._/:*?~$`"])*) ;

token Assign (letter (letter | digit | '_')* '='
              (letter | digit | ["-._/:*?~$`"])*) ;
```

**Design decisions:**
- Backtick `` ` `` is included in `Word` as a space-placeholder for `"..."` preprocessing
- `-` placed first in `["-..."]` so it is a literal hyphen, not a range separator
- `Assign` declared before `Word` so `x=hello` matches `Assign` first

### Complete Grammar Structure

```
Input     → [Job]
Job       → Condition
          | Condition "&"
          | Assign
          | "if" Condition "then" [Job] OptElse "fi"

OptElse   → (empty)
          | "else" [Job]
          | "elif" Condition "then" [Job] OptElse

Condition → NegCmd
          | NegCmd "&&" Condition
          | NegCmd "||" Condition

NegCmd    → CommandLine
          | "!" NegCmd
          | "time" NegCmd
          | "(" [Job] ")"
          | "{" [Job] "}"

CommandLine → Pipeline OptRedir

OptRedir  → (empty) | ">>" Word | ">" Word | "<" Word
          | "<" Word ">" Word | ">" Word "<" Word

Pipeline  → CommandPart | CommandPart "|" Pipeline

CommandPart → Word [Word]
```

---

## 3. Features Implemented

### 3.1 Grammar-Based Parsing (Base)

**What changed:** The hand-rolled `preprocess() → tokenise() → separate_commands()` pipeline is replaced by `psInput(line)` which invokes the BNFC-generated LR parser. Every other week's execution code (registry dispatch, fork/exec, run_pipeline, pthread dispatch) is completely unchanged.

**Entry point:** `psInput(const char*)` — parses a string (one REPL line) into an `Input` AST.

---

### 3.2 Quoted Strings `"hello world"`

**Problem:** BNFC's lexer splits on whitespace, so `"hello world"` cannot be a single token natively.

**Solution — C pre-processing before `psInput()`:**
1. `preprocess_quotes(line)` scans the line before the BNFC parser sees it
2. Strips surrounding `"` characters
3. Replaces spaces INSIDE `"..."` with backtick `` ` `` (space placeholder)
4. Backtick is in the `Word` token charset — `"hello world"` → single token `hello`world`
5. After parsing, `expand_and_unquote()` replaces `` ` `` → space in every `argv[]` entry

```
"hello world" → hello`world  (pre-process)
                ↓
             Word token: hello`world
                ↓
             expand_and_unquote → "hello world"  (argv[1])
```

---

### 3.3 Variable Assignment and Expansion

| Feature | Implementation |
|---|---|
| `x=value` | `AssignJob` grammar production → `var_set(name, value)` |
| `$x` expansion | `expand_and_unquote()` calls `expand_vars()` on every Word |
| `$?` last exit status | `g_last_status` updated after each foreground job |
| `$$` shell PID | `getpid()` in `expand_vars()` |
| `${var}` brace form | Parsed by `expand_vars()` alongside `$name` |
| Environment fallback | `getenv(name)` called when `var_store` lookup misses |

---

### 3.4 if / then / elif / else / fi

**Grammar:** A recursive `OptElse` category supports any number of `elif` branches naturally:

```
NoElse.   OptElse ::= ;
ElsePart. OptElse ::= "else" [Job] ;
ElifPart. OptElse ::= "elif" Condition "then" [Job] OptElse ;
```

**Evaluator:** `eval_optelse(OptElse oe, int cond_failed)` — walks the chain recursively. If `cond_failed == 0`, the branch was already taken; skip. Otherwise try the next elif/else.

---

### 3.5 AND List `&&` and OR List `||`

**Grammar:** Right-recursive `Condition` at the level above `CommandLine`:

```
CondSingle. Condition ::= NegCmd ;
CondAnd.    Condition ::= NegCmd "&&" Condition ;
CondOr.     Condition ::= NegCmd "||" Condition ;
```

**Evaluation (short-circuit):**
- `&&` — run LEFT; if exit 0 (success), run RIGHT; otherwise stop
- `||` — run LEFT; if exit non-0 (failure), run RIGHT; otherwise stop

**Field order note:** BNFC stores `CondAnd`/`CondOr` fields in reverse grammar order:
- `condand_.negcmd_` = LEFT operand (NegCmd)
- `condand_.condition_` = RIGHT operand (Condition)

---

### 3.6 NOT `!`

**Grammar:** `NotCL. NegCmd ::= "!" NegCmd` — applies to any `NegCmd`, including pipelines, subshells, and groups.

```
! cmd               → NotCL (PlainCL cmd)
! ls | grep c       → NotCL (PlainCL (Pipeline ls|grep))
! ( subshell )      → NotCL (Subshell ...)
! { group }         → NotCL (Group ...)
! ! cmd             → double negation (supported)
```

**Evaluation:** Run inner `NegCmd`; return `(rc == 0) ? 1 : 0`.

---

### 3.7 Subshell `( CMDS )`

**Grammar:** `Subshell. NegCmd ::= "(" [Job] ")"` — at `NegCmd` level so it composes with `&&`, `||`, `!`, `time`.

**Evaluation:**
1. `fflush(NULL)` — flush all stdio buffers before forking
2. `fork()` — create child process
3. Child: restore default signals, call `eval_run_listjob()`, `exit(rc)`
4. Parent: `waitpid()` and return child's exit code

**Key behaviour:** Changes to the working directory and shell variables inside `()` do **not** propagate to the parent shell.

---

### 3.8 Group Command `{ CMDS }`

**Grammar:** `Group. NegCmd ::= "{" [Job] "}"` — at `NegCmd` level.

**Evaluation:** Runs all jobs via `eval_run_listjob()` in the **current process** — no fork. Changes to `cd` and variables **do** propagate to the parent.

**Syntax note:** No trailing `;` before `}` — write `{ cmd1 ; cmd2 }` not `{ cmd1 ; cmd2 ; }`. This is because `separator nonempty Job ";"` puts `;` between jobs, not after the last one.

---

### 3.9 Arithmetic Expansion `$(( expr ))`

**Pre-processing approach:** `preprocess_arith(line)` scans the input line for `$((` patterns and evaluates them before `psInput()` sees the line. No grammar changes needed.

**Closing `))` detection:**
```
Scan with inner-paren depth tracking:
  '(' → depth++
  ')' with depth > 0 → depth--
  ')' with depth == 0 AND next char is ')' → end of $((expr))
```

**Arithmetic evaluator:** Recursive descent parser (`ap_expr → ap_term → ap_factor`) supporting:
- Integer literals
- Variable references: `$name` and bare `name`
- Unary `+` and `-`
- Binary `+`, `-`, `*`, `/`, `%` with correct precedence
- Parenthesised sub-expressions

**Processing order:**
```
preprocess_arith()   →   preprocess_quotes()   →   psInput()
$((2+3)) → 5             "hello" → hello`...       BNFC parse
```

**Known limitations:**
- Negative results (e.g., `-7`) may be treated as flags by registry commands that use argtable3
- `NAME=VALUE` as a command argument (e.g., `echo sum=5`) may be tokenised as `Assign` causing a parse error — use a variable instead: `r=5 ; echo sum=$r`

---

### 3.10 Pipeline Negation `! PIPELINE`

Already supported by the existing `NotCL` grammar — no additional changes required. `NegCmd → PlainCL → CommandLine → Pipeline` absorbs the full pipe chain.

```
! ls | grep c    →   NotCL (PlainCL (Pipe ls grep_c))
                      eval_negcmd → eval_cmdline → run_pipeline
                      exit code inverted
```

---

### 3.11 time Command `time PIPELINE`

**Grammar:** `TimeCmd. NegCmd ::= "time" NegCmd` — same level as `!` so it composes freely.

**Evaluation:**
1. `clock_gettime(CLOCK_MONOTONIC, &t0)` before running
2. `eval_negcmd(inner)` — runs the timed NegCmd
3. `clock_gettime(CLOCK_MONOTONIC, &t1)` after
4. `fprintf(stderr, "\nreal\t%.3fs\n", elapsed)` — stderr keeps stdout clean for pipelines

**Composition examples:**
- `time ls | grep c` — times the full pipeline
- `time ls && echo done` — times only `ls`
- `! time /usr/bin/false` — times, then negates exit code
- `time ( cmd1 ; cmd2 )` — times an entire subshell

---

### 3.12 @ Natural Language Handler

**Grammar:** Not a grammar production — handled as a special case in `bnfc_repl()` before `psInput()` is called.

**Flow:**
1. Line starts with `@` → extract query string
2. Fork and exec `./mysh_llm` with query as argv[1]
3. Read exactly one line from its stdout — the suggested command
4. Display suggestion and ask user for confirmation (`y/N`)
5. If confirmed: `preprocess_arith() → preprocess_quotes() → psInput()` on the suggestion

**`mysh_llm` stub:** Python script with keyword-based suggestions. Designed to be replaced with a real LLM integration.

---

### 3.13 `--commands-json` Catalog

Running `./aishell --commands-json` emits all 32 registry commands as a JSON array using `for_each_command()` from the existing registry. A `json_puts()` helper escapes `"`, `\`, `\n`, `\t` so the output is always valid JSON.

---

## 4. AST Evaluator Functions

| Function | Purpose |
|---|---|
| `eval_input(Input)` | Walk top-level `[Job]` list |
| `eval_job(Job)` | Dispatch on job kind: FG/BG/Assign/If |
| `eval_condition(Condition)` | AND/OR short-circuit evaluation |
| `eval_negcmd(NegCmd)` | Plain/Not/Time/Subshell/Group dispatch |
| `eval_optelse(OptElse, failed)` | Recursive elif/else chain |
| `eval_cmdline(CommandLine)` | Build cmd_t, call bnfc_run_cmd or run_pipeline |
| `eval_pipeline_ast(Pipeline, cmds[], idx)` | Recursively fill cmd_t array, set pipe_next |
| `eval_redir(OptRedir, cmds[], n)` | Set stdin_file/stdout_file/stdout_append |
| `eval_cmdpart(CommandPart, cmd_t*)` | Build argv[] with var expansion + glob |
| `eval_run_listjob(ListJob)` | Run every job in a list |
| `eval_condition_cl(CommandLine)` (internal) | Renamed `eval_cmdline` |
| `bnfc_run_cmd(cmd_t*)` | Route through `execute_command()` for in-process redirect handling |
| `expand_vars(word)` | Replace `$x`, `$?`, `$$`, `${var}` in Word tokens |
| `expand_and_unquote(word)` | `expand_vars` + replace `` ` `` → space |
| `append_word_glob(argv, argc, word)` | Expand `*`/`?` via `glob(GLOB_NOCHECK)` |
| `preprocess_quotes(line)` | Strip `"`, replace internal spaces with `` ` `` |
| `preprocess_arith(line)` | Find `$((expr))`, evaluate, substitute result |
| `arith_eval(expr_start, **endp)` | Evaluate one arithmetic expression |
| `ap_expr / ap_term / ap_factor` | Recursive descent arithmetic parser |

---

## 5. Issues Found and Fixed

| # | Issue | Root Cause | Fix |
|---|-------|-----------|-----|
| 1 | `entry` keyword rejected | BNFC 2.9.5 uses `entrypoints` | Changed to `entrypoints` |
| 2 | `[._/\-]` char class rejected | BNFC 2.9.5 requires double-quoted strings inside `[]` | Changed to `["-._/:*?~$"]` |
| 3 | `"_"` in alternation rejected | Double quotes only valid in grammar rules; use `'_'` in token alternation | Changed to single-quoted `'_'` |
| 4 | `separator` macro rejected with `ListJob` name | BNFC generates list category automatically as `[Cat]` | Removed explicit list name |
| 5 | `isalnum` undeclared | `<ctype.h>` not included | Added `#include <ctype.h>` |
| 6 | `bnfc_repl` implicit declaration error | Function defined after `main()` inside `#ifdef` | Added forward declarations before `main()` |
| 7 | I/O redirections not applied | BNFC evaluator called `lsh_execute()` directly, bypassing `execute_command()`'s dup/restore | Changed to `bnfc_run_cmd()` wrapping `execute_command()` |
| 8 | `--commands-json` silently ignored | Mode 2 (`argc > 1`) intercepted it | Moved `--commands-json` check before Mode 2 |
| 9 | `--commands-json` invalid JSON | `long_help` strings contain unescaped `"` and `\n` | Added `json_puts()` with proper JSON escaping |
| 10 | Glob patterns not expanded | `eval_cmdpart` built argv from raw tokens without `glob()` | Added `append_word_glob()` using `glob(GLOB_NOCHECK)` |
| 11 | `_DAMP` / `_DBAR` / `_BANG` undeclared | `flex`/`bison` invoked without `-Pgrammar_`/`-pgrammar_` prefix flags | Added correct prefix flags to Makefile |
| 12 | `{ ls ; }` parse error | Trailing `;` before `}` not allowed by `separator nonempty` | Documented: write `{ ls }` not `{ ls ; }` |
| 13 | `! ( subshell )` no output | `NotCL` applied `!` to `CommandLine` — `( )` is `NegCmd` not `CommandLine` | Changed to `NotCL. NegCmd ::= "!" NegCmd` |
| 14 | Arithmetic expansion not running | Binary was stale after `make bnfc-gen` | `make clean && make` forced complete rebuild |
| 15 | `mysh_llm` not found | `execlp("mysh_llm")` searched `$PATH` — script is in `./` | Changed to `execl("./mysh_llm", ...)` first, then PATH fallback |
| 16 | `grep .c` matched non-.c files | `.` is regex "any character" | Changed to `grep -F .c` (fixed-string match) |
| 17 | `time ( subshell )` parse error in stale TestGrammar | TestGrammar binary not rebuilt after grammar change | `make -C bnfc TestGrammar` |

---

## 6. Grammar Evolution Summary

| Phase | Rules | Features Added |
|---|---|---|
| Base | 19 | Commands, pipelines, redirection, background, semicolons, variables, if/then/fi |
| +Quoted strings | 19 | `"hello world"` via C pre-processing (no grammar change) |
| +if/elif/else | 22 | `OptElse` category: `NoElse`, `ElsePart`, `ElifPart` |
| +&&, \|\|, ! | 27 | `Condition` and `NegCmd` categories; `CondAnd`, `CondOr`, `NotCL` |
| +Subshell, Group | 29 | `Subshell` and `Group` as `NegCmd` productions |
| +time | 30 | `TimeCmd. NegCmd ::= "time" NegCmd` |

---

## 7. Test Suite — `test_week7.sh`

26 checks across 12 sections — all passing:

| Section | Coverage |
|---|---|
| 1. Basic command | `echo hello`, `ls -l /tmp` |
| 2. Pipeline | `echo \| grep`, `echo \| wc -c` |
| 3. I/O Redirection | `>`, `>>`, `<` |
| 4. Background job | `sleep &` |
| 5. Semicolons | Three jobs in sequence |
| 6. Variable expansion | `x=world ; echo $x` |
| 7. Special variables | `$?`, `$$` |
| 8. if/then/fi | Success and failure branches |
| 9. Syntax error rejection | Invalid input, shell continues |
| 10. No regression | `help`, `cd/pwd`, exit codes |
| 11. --commands-json | Valid JSON with 32 commands |
| 12. Glob patterns | `ls *.c` expands correctly |

**Final result: 26 / 26 passed**

---

## 8. Makefile Targets

| Target | Purpose |
|---|---|
| `make bnfc-gen` | Run `bnfc --c -m Grammar.cf` to generate sources (run once, or after Grammar.cf changes) |
| `make` | Build `./aishell` with BNFC parser (`-DUSE_BNFC -Ibnfc`) |
| `make bnfc-test` | Build BNFC's own `TestGrammar` AST pretty-printer |
| `make clean` | Remove `./aishell` |
| `make bnfc-clean` | Remove BNFC object files |

**Build sequence after Grammar.cf changes:**
```sh
make bnfc-gen
cd bnfc && bison -t -pgrammar_ Grammar.y -o Parser.c
cd bnfc && flex -Pgrammar_ -oLexer.c Grammar.l
make
```

---

## 9. Build and Test

```sh
cd week3
make bnfc-gen
cd bnfc && bison -t -pgrammar_ Grammar.y -o Parser.c && flex -Pgrammar_ -oLexer.c Grammar.l && cd ..
make
bash test_week7.sh        # 26 / 26
bash test_week6.sh        # 23 / 23 (no regression)
bash test_week5.sh        # 25 / 25 (no regression)
bash demo_week7.sh        # full feature demo
./aishell --commands-json # 32-command JSON catalog
```

---

## 10. Known Limitations

| Limitation | Workaround |
|---|---|
| Negative arithmetic results (`-7`) treated as flags by registry `echo` | Store in variable: `r=$((-7)) ; /usr/bin/echo $r` |
| `echo NAME=VALUE` — `NAME=VALUE` tokenised as `Assign` | Use variable: `r=value ; echo name=$r` |
| No trailing `;` allowed before `}` in group commands | Write `{ cmd1 ; cmd2 }` not `{ cmd1 ; cmd2 ; }` |
| Double quotes with `=` inside (e.g., `"key=val"`) may cause parse error | Separate: `k=key ; v=val ; echo $k=$v` |
| `mysh_llm` is a stub — AI suggestions are keyword-based only | Replace `mysh_llm` with a real LLM API call |

---

*Work log prepared for Week 7 of the AiShell project.*
