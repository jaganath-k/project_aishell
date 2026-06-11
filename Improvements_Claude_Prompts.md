# AiShell — Improvement Prompts (Step-by-Step)

Reference guide for improving AiShell beyond Week 8.
Each step is a self-contained prompt — paste it directly into Claude Code.

**Working directory for all steps:** `/home/jagan/aishell/week3`
**Branch convention:** create a new branch before each phase (`git checkout -b week9` etc.)

---

## PHASE 1 — Grammar Fixes (Highest ROI — no new files)

These are one-line changes to `bnfc/Grammar.cf` that immediately unblock
real Unix commands that currently fail to parse.

---

### Step 1 — Widen Word token (add `+`, `%`, `,`)

```
We are working in /home/jagan/aishell/week3 on the AiShell project.

The BNFC grammar is at bnfc/Grammar.cf. The current Word token definition is:

  token Word ( (letter | digit | ["-._/:*?~$`"])
               (letter | digit | ["-._/:*?~$`"])* ) ;

This excludes the characters +  %  ,  which means commands like
  find / -size +10M          (+ in argument)
  ps -eo pid,%cpu,%mem       (% in field name)
  ps -eo pid,ppid,cmd        (, in field list)
all fail to parse with "syntax error at '+'" etc.

Task: Edit bnfc/Grammar.cf to add +  %  , to the Word token character class.
The new definition should be:

  token Word ( (letter | digit | ["-._/:*?~$`+%,"])
               (letter | digit | ["-._/:*?~$`+%,"])* ) ;

After editing:
1. Regenerate the lexer/parser: cd bnfc && bnfc --c -m Grammar.cf
2. Rebuild: cd .. && make clean && make
3. Verify zero warnings.
4. Test inside aishell:
     ps -eo pid,%cpu,%mem --sort=-%cpu | head -5
     find . -size +1k -type f
   Both should parse and execute without "syntax error".
```

---

### Step 2 — Add stderr redirect (`2>` and `2>>`)

```
We are working in /home/jagan/aishell/week3 on the AiShell project.
The BNFC grammar is at bnfc/Grammar.cf.

The current OptRedir rule handles >, >>, < but NOT stderr (2> and 2>>).
Commands like:
  find / -name "*.log" 2>/dev/null
  make 2>>build_errors.log
currently fail because "2>" is tokenised as Word("2") then OutRedir(">") which
leaves the rest of the pipeline broken.

Task:
1. Read bnfc/Grammar.cf to find the OptRedir rule block.
2. Add two new productions BEFORE the existing OutRedir line
   (longer tokens must come first so the lexer prefers them):

     ErrAppRedir. OptRedir ::= "2>>" Word ;
     ErrRedir.    OptRedir ::= "2>"  Word ;

3. Also add combined stdout+stderr redirect:
     BothRedir.   OptRedir ::= "&>"  Word ;

4. Add corresponding cases in the BNFC AST executor inside aishell_main.c
   (the switch/if block that handles AppendRedir, OutRedir, InRedir) so that:
   - ErrRedir    → dup2(open(file, O_WRONLY|O_CREAT|O_TRUNC), STDERR_FILENO)
   - ErrAppRedir → dup2(open(file, O_WRONLY|O_CREAT|O_APPEND), STDERR_FILENO)
   - BothRedir   → redirect both STDOUT and STDERR to the file

5. Regenerate (cd bnfc && bnfc --c -m Grammar.cf), rebuild (make clean && make).
6. Test inside aishell:
     find /root -name "*.c" 2>/dev/null
     ls /nonexistent 2>>errors.log && cat errors.log
```

---

### Step 3 — Command substitution `$(...)`

```
We are working in /home/jagan/aishell/week3 on the AiShell project.
The BNFC grammar is at bnfc/Grammar.cf.

Currently backtick `...` is handled by preprocessing (replaced with a
space-placeholder), but the modern $(...) form is not supported.

Task:
1. Add a CmdSubst token to Grammar.cf that matches $( ... ) as a single Word:

     token CmdSubst ( '$' '(' ( char - [")"] )* ')' ) ;

2. In the CommandPart rule, extend [Word] to also accept CmdSubst:
   Change:
     Cmd. CommandPart ::= Word [Word] ;
   To use a new Arg category that is either a Word or CmdSubst:

     WordArg.    Arg ::= Word ;
     SubstArg.   Arg ::= CmdSubst ;
     Cmd.        CommandPart ::= Word [Arg] ;
     separator Arg " " ;

3. In the AST executor (aishell_main.c), when building argv[] from Arg nodes,
   detect SubstArg, strip the $( ) wrapper, run the inner command via popen(),
   capture its stdout, trim the trailing newline, and substitute the result
   as the argument string.

4. Regenerate and rebuild.
5. Test inside aishell:
     echo $(date)
     echo $(uname -r)
     ls $(pwd)
```

---

## PHASE 2 — Week 9 Commands (Text Processing)

Each command follows the APPANATOMY pattern:
- Create `cmd_<name>.c`
- Add to `SRCS` in Makefile
- Add `extern void register_<name>_command(void);` in aishell_main.c
- Call `register_<name>_command();` inside `register_all_commands()`

The pattern for every cmd_<name>.c:
- Define `build_<name>_argtable()` helper with all args as out-params + `void ***argtable_out`
- Define `<name>_print_usage(FILE *fp)`
- Define `cmd_<name>_run(int argc, char **argv)`
- Define and export `cmd_spec_t cmd_<name>_spec` and `register_<name>_command()`
- Always include `--help` and `--help-json` in every argtable

---

### Step 4 — `uniq` command

```
We are working in /home/jagan/aishell/week3 on the AiShell project.
This project uses argtable3 (installed at /usr/local/include/argtable3.h).
Every command follows the APPANATOMY pattern documented in CLAUDE.md.

Look at cmd_sort.c as a reference for a similar text-processing command.

Task: implement the uniq built-in command.

Flags to support:
  -c / --count       prefix each output line with its repeat count
  -d / --repeated    only print duplicate lines (lines that appear more than once)
  -u / --unique      only print lines that are NOT repeated
  -i / --ignore-case fold lowercase to uppercase before comparison
  FILE (optional positional) — read from FILE if given, else stdin

Behaviour:
  Reads lines sequentially (like POSIX uniq — only collapses ADJACENT duplicates).
  When no FILE is given, read from stdin so it works in pipelines:
    sort file.txt | uniq -c | sort -rn

Implementation steps:
1. Create cmd_uniq.c following the build_uniq_argtable() pattern.
2. Add cmd_uniq.c to SRCS in Makefile.
3. Add extern declaration and register call in aishell_main.c.
4. Build: make clean && make  (zero warnings required).
5. Test:
     printf "apple\napple\nbanana\napple\n" | ./aishell uniq
     printf "apple\napple\nbanana\napple\n" | ./aishell uniq -c
     printf "apple\napple\nbanana\napple\n" | ./aishell uniq -d
     ./aishell uniq --help-json
```

---

### Step 5 — `cut` command

```
We are working in /home/jagan/aishell/week3 on the AiShell project.
Every command follows the APPANATOMY pattern in CLAUDE.md.
Look at cmd_wc.c as a reference.

Task: implement the cut built-in command.

Flags to support:
  -d DELIM / --delimiter=DELIM   field delimiter (default: TAB)
  -f LIST  / --fields=LIST       select fields (e.g. 1,3 or 1-3 or 1-3,5)
  -c LIST  / --characters=LIST   select character positions
  FILE... (optional positional list) — read from each FILE or stdin if none

Behaviour:
  -f and -c are mutually exclusive; error if both given or neither given.
  LIST format: comma-separated ranges like cut(1): 1, 1-3, 1-3,5, -3, 3-
  Output each selected field/char separated by the delimiter (for -f)
  or concatenated (for -c).

Implementation steps:
1. Create cmd_cut.c with a parse_list() helper that turns "1,3-5" into a
   boolean selected[MAX_FIELDS] array.
2. Add to Makefile SRCS and register in aishell_main.c.
3. Build and verify zero warnings.
4. Test:
     echo "one:two:three:four" | ./aishell cut -d: -f2
     echo "one:two:three:four" | ./aishell cut -d: -f1,3
     echo "abcdef" | ./aishell cut -c2-4
     cat /etc/passwd | ./aishell cut -d: -f1 | ./aishell sort | ./aishell uniq
     ./aishell cut --help-json
```

---

### Step 6 — `tr` command

```
We are working in /home/jagan/aishell/week3 on the AiShell project.
Every command follows the APPANATOMY pattern in CLAUDE.md.

Task: implement the tr built-in command.
tr reads from stdin only (POSIX tr takes no file arguments).

Flags to support:
  -d / --delete          delete characters in SET1 from input (no SET2 needed)
  -s / --squeeze-repeats replace each sequence of a repeated character in SET1
                         with a single occurrence
  -c / --complement      use the complement of SET1

Positional arguments:
  SET1   (always required)
  SET2   (required unless -d is given)

SET syntax to support (minimal but useful subset):
  Literal characters: abc
  Ranges:             a-z  A-Z  0-9
  Named classes:      [:upper:]  [:lower:]  [:digit:]  [:space:]  [:alpha:]

Behaviour:
  Without flags: replace each char in SET1 with the corresponding char in SET2
    echo "Hello World" | tr 'a-z' 'A-Z'
  With -d: delete each char that appears in SET1
    echo "Hello 123" | tr -d '0-9'
  With -s: squeeze repeated chars
    echo "aabbcc" | tr -s 'a-z'

Implementation steps:
1. Create cmd_tr.c with an expand_set() helper that expands ranges and
   named classes into a flat 256-byte boolean/map array.
2. tr always reads stdin → transforms → writes stdout. No file arg.
3. Add to Makefile SRCS and register in aishell_main.c.
4. Build and verify zero warnings.
5. Test:
     echo "Hello World" | ./aishell tr 'a-z' 'A-Z'
     echo "Hello 123 World" | ./aishell tr -d '0-9'
     echo "hello   world" | ./aishell tr -s ' '
     printf "line1\r\nline2\r\n" | ./aishell tr -d '\r'
     ./aishell tr --help-json
```

---

### Step 7 — `grep` command

```
We are working in /home/jagan/aishell/week3 on the AiShell project.
Every command follows the APPANATOMY pattern in CLAUDE.md.
We already have cmd_rg.c (recursive grep). This is a simpler grep
that operates on stdin or specific files without defaulting to recursive.

Task: implement the grep built-in command using POSIX regex (regcomp/regexec).

Flags to support:
  -v / --invert-match    print lines that do NOT match
  -n / --line-number     prefix each matching line with its line number
  -c / --count           print only the count of matching lines
  -i / --ignore-case     case-insensitive matching
  -l / --files-with-matches  print only filenames that contain a match
  -r / --recursive       recurse into directories (like rg)
  PATTERN  (required positional)
  FILE...  (optional; read stdin if none given)

Behaviour:
  Uses POSIX ERE (REG_EXTENDED). With -i use REG_ICASE.
  Exit code: 0 if any match found, 1 if no match, 2 on error.
  This makes it composable in && / || chains.

Implementation steps:
1. Create cmd_grep.c. Reuse the recursive directory walk logic from cmd_rg.c
   for the -r flag (copy the opendir/readdir loop).
2. Add to Makefile SRCS and register in aishell_main.c.
3. Build and verify zero warnings.
4. Test:
     cat /etc/passwd | ./aishell grep root
     cat /etc/passwd | ./aishell grep -v root
     cat /etc/passwd | ./aishell grep -n root
     cat /etc/passwd | ./aishell grep -c root
     ./aishell grep -ri "TODO" .
     ./aishell grep --help-json
```

---

### Step 8 — `diff` command

```
We are working in /home/jagan/aishell/week3 on the AiShell project.
Every command follows the APPANATOMY pattern in CLAUDE.md.

Task: implement the diff built-in command.

Flags to support:
  -u / --unified         output N lines of unified context (default 3)
  -i / --ignore-case     ignore case differences
  -q / --brief           report only whether files differ, not how
  -b / --ignore-space-change  ignore changes in the amount of whitespace
  FILE1  (required positional)
  FILE2  (required positional)

Behaviour:
  Compare FILE1 and FILE2 line by line.
  Default output: traditional diff format (lines starting with < and >).
  With -u: unified diff format with @@ hunk headers.
  Exit code: 0 = identical, 1 = different, 2 = error.

Implementation approach:
  Read both files into line arrays. Use a basic LCS (longest common
  subsequence) algorithm to find differing lines. This avoids shelling
  out to /usr/bin/diff and keeps the command self-contained.
  For -u output, track line numbers and emit @@ -L,S +L,S @@ headers.

Implementation steps:
1. Create cmd_diff.c with a static lcs() helper (2D DP array, max 4096 lines).
2. Add to Makefile SRCS and register in aishell_main.c.
3. Build and verify zero warnings.
4. Test:
     echo -e "a\nb\nc" > /tmp/f1.txt && echo -e "a\nX\nc" > /tmp/f2.txt
     ./aishell diff /tmp/f1.txt /tmp/f2.txt
     ./aishell diff -u /tmp/f1.txt /tmp/f2.txt
     ./aishell diff -q /tmp/f1.txt /tmp/f2.txt
     ./aishell diff /tmp/f1.txt /tmp/f1.txt  # should exit 0, no output
     ./aishell diff --help-json
```

---

### Step 9 — `tee` command

```
We are working in /home/jagan/aishell/week3 on the AiShell project.
Every command follows the APPANATOMY pattern in CLAUDE.md.

Task: implement the tee built-in command.
tee reads stdin and writes to both stdout AND one or more files simultaneously.

Flags to support:
  -a / --append    append to files rather than overwriting
  FILE...          one or more output files (required)

Behaviour:
  Read stdin in a loop, write each chunk to stdout AND to each FILE.
  With -a: open files with O_APPEND.
  Without -a: open files with O_TRUNC.
  This makes long pipelines debuggable:
    cat big.log | grep ERROR | tee errors.txt | wc -l

Implementation steps:
1. Create cmd_tee.c. Open all file descriptors before starting the read loop.
   Use a 4096-byte read buffer. Write to stdout first, then each fd in order.
   Close all fds on EOF or error.
2. Add to Makefile SRCS and register in aishell_main.c.
3. Build and verify zero warnings.
4. Test (run inside ./aishell):
     echo "hello world" | tee /tmp/tee_test.txt
     cat /tmp/tee_test.txt
     echo "appended" | tee -a /tmp/tee_test.txt
     cat /tmp/tee_test.txt
     ls -la | tee /tmp/ls_out.txt | wc -l
     tee --help-json
```

---

### Step 10 — Week 9 regression check

```
We are working in /home/jagan/aishell/week3 on the AiShell project.

We have just implemented Phase 1 grammar fixes (Word token, 2>, &>) and
Phase 2 commands (uniq, cut, tr, grep, diff, tee).

Task: run all existing test suites and confirm no regressions.

1. Build cleanly: make clean && make
   Confirm: zero warnings with -Wall -Wextra.

2. Run all regression suites:
   bash test_week8.sh
   bash test_week7.sh
   bash test_week6.sh
   bash test_week5.sh

3. For each new command run a quick smoke test inside ./aishell:
   echo "aaa bbb aaa ccc aaa" | tr ' ' '\n' | sort | uniq -c | sort -rn
   ls -la | cut -d' ' -f1 | grep -v '^$' | sort | uniq
   echo "Hello World" | tr 'A-Z' 'a-z' | tee /tmp/smoke.txt
   cat /tmp/smoke.txt | diff - /tmp/smoke.txt

4. Report: total pass/fail count for each suite and confirm new commands work.
   If any regression is found, fix it before proceeding to Phase 3.
```

---

## PHASE 3 — Week 10 Commands (Filesystem Deep Dive)

---

### Step 11 — `du` command

```
We are working in /home/jagan/aishell/week3 on the AiShell project.
Every command follows the APPANATOMY pattern in CLAUDE.md.
Look at cmd_stat.c as a reference for filesystem-related commands.

Task: implement the du (disk usage) built-in command.

Flags to support:
  -h / --human-readable   print sizes like 4.2K, 15M, 2.3G
  -s / --summarize        display only a total for each argument (no subdirs)
  -d N / --max-depth=N    print totals for directories at most N levels deep
  -a / --all              include files, not just directories
  PATH...                 directories to measure (default: current directory)

Behaviour:
  Walk the directory tree with opendir/readdir/stat, accumulate st_blocks * 512
  bytes per entry. With -h, format using the largest appropriate unit.
  With -s, only print the grand total per top-level argument.
  With -d N, print subtotals up to depth N then suppress deeper entries.

Implementation steps:
1. Create cmd_du.c with a recursive static du_walk() helper that takes
   (path, depth, max_depth, flags) and returns total bytes.
2. Add to Makefile SRCS and register in aishell_main.c.
3. Build and verify zero warnings.
4. Test (inside ./aishell):
     du -sh /tmp
     du -h -d 1 /var
     du -a -h /etc | sort -rh | head -10
     du --help-json
```

---

### Step 12 — `df` command

```
We are working in /home/jagan/aishell/week3 on the AiShell project.
Every command follows the APPANATOMY pattern in CLAUDE.md.

Task: implement the df (disk free) built-in command.

Flags to support:
  -h / --human-readable   print sizes in human-readable form (K, M, G)
  -T / --print-type       include filesystem type column in output
  PATH...                 if given, show only the filesystem containing PATH

Behaviour:
  Use getmntent() (from <mntent.h>) to iterate /proc/mounts or /etc/mtab.
  For each entry call statvfs() to get f_blocks, f_bfree, f_bavail, f_frsize.
  Compute: Total = f_blocks * f_frsize, Used = (f_blocks-f_bfree)*f_frsize,
           Avail = f_bavail * f_frsize, Use% = (Used/Total)*100.
  Print a table with columns: Filesystem | Type | Size | Used | Avail | Use% | Mounted on.
  Skip pseudo-filesystems (proc, sysfs, devtmpfs, tmpfs) unless they appear
  in the PATH filter.

Implementation steps:
1. Create cmd_df.c. Use setmntent("/proc/mounts","r") + getmntent() loop.
2. Add to Makefile SRCS and register in aishell_main.c.
3. Build and verify zero warnings.
4. Test (inside ./aishell):
     df -h
     df -hT
     df -h /home
     df --help-json
```

---

### Step 13 — `ln` command

```
We are working in /home/jagan/aishell/week3 on the AiShell project.
Every command follows the APPANATOMY pattern in CLAUDE.md.

Task: implement the ln (link) built-in command.

Flags to support:
  -s / --symbolic    create a symbolic link instead of a hard link
  -f / --force       remove destination file if it already exists
  -v / --verbose     print a line for each link created
  TARGET             the existing file to link to
  LINK_NAME          the name of the new link to create

Behaviour:
  Without -s: call link(TARGET, LINK_NAME) — hard link.
  With -s: call symlink(TARGET, LINK_NAME) — soft link.
  With -f: if LINK_NAME already exists, unlink it first then create the link.
  With -v: print "TARGET -> LINK_NAME" after creation.
  Error if TARGET does not exist (for hard links).

Implementation steps:
1. Create cmd_ln.c.
2. Add to Makefile SRCS and register in aishell_main.c.
3. Build and verify zero warnings.
4. Test (inside ./aishell):
     ln -s /etc/hostname /tmp/hostname_link
     ls -la /tmp/hostname_link
     cat /tmp/hostname_link
     ln -sf /etc/os-release /tmp/hostname_link
     ln --help-json
```

---

### Step 14 — `chmod` command

```
We are working in /home/jagan/aishell/week3 on the AiShell project.
Every command follows the APPANATOMY pattern in CLAUDE.md.

Task: implement the chmod built-in command.

Flags to support:
  -R / --recursive   change permissions recursively for directories
  -v / --verbose     print a diagnostic for each file processed
  MODE               octal (755, 644, 600) or symbolic (u+x, go-w, a=r)
  FILE...            one or more files/directories

Behaviour:
  Octal mode: parse MODE as a base-8 integer, pass directly to chmod(2).
  Symbolic mode: parse [ugoa][+-=][rwxXst] and compute the new mode bits
    from the existing stat() mode using the same rules as POSIX chmod.
  With -R: use opendir/readdir to recurse into directories.
  With -v: print "mode of 'FILE' changed from XXXX to YYYY".

Implementation steps:
1. Create cmd_chmod.c with a parse_symbolic_mode(const char *str, mode_t old)
   helper that returns the new mode_t.
2. Add recursive walk helper for -R.
3. Add to Makefile SRCS and register in aishell_main.c.
4. Build and verify zero warnings.
5. Test (inside ./aishell):
     touch /tmp/testfile_chmod
     chmod 644 /tmp/testfile_chmod && stat /tmp/testfile_chmod
     chmod u+x /tmp/testfile_chmod && stat /tmp/testfile_chmod
     chmod -v go-w /tmp/testfile_chmod
     chmod --help-json
```

---

### Step 15 — `chown` command

```
We are working in /home/jagan/aishell/week3 on the AiShell project.
Every command follows the APPANATOMY pattern in CLAUDE.md.

Task: implement the chown built-in command.

Flags to support:
  -R / --recursive   change ownership recursively
  -v / --verbose     print a diagnostic for each file changed
  OWNER[:GROUP]      new owner and optional group (e.g. root, jagan:users, :www-data)
  FILE...            one or more targets

Behaviour:
  Parse OWNER[:GROUP]: use getpwnam() for the user lookup and getgrnam()
  for the group. If GROUP is omitted keep the existing group (-1 for gid).
  Call lchown(2) (not chown — so symlinks themselves are affected, not targets).
  With -R: recurse directories with opendir/readdir.
  With -v: print "changed ownership of FILE to OWNER:GROUP".

Implementation steps:
1. Create cmd_chown.c. Parse the OWNER:GROUP string carefully — the colon
   is optional and GROUP alone (:grp) means change only the group.
2. Add to Makefile SRCS and register in aishell_main.c.
3. Build and verify zero warnings.
4. Test (inside ./aishell — run as root or with sudo):
     touch /tmp/chown_test
     chown root /tmp/chown_test
     stat /tmp/chown_test
     chown nobody:nogroup /tmp/chown_test
     chown --help-json
```

---

### Step 16 — `sleep` command

```
We are working in /home/jagan/aishell/week3 on the AiShell project.
Every command follows the APPANATOMY pattern in CLAUDE.md.

Task: implement the sleep built-in command.

Arguments:
  NUMBER[SUFFIX]...   one or more durations (they are summed)
  SUFFIX: s (seconds, default), m (minutes), h (hours), d (days)

Behaviour:
  Parse each argument, multiply by its suffix factor, sum all durations,
  call nanosleep() in a loop that retries on EINTR so sleep survives SIGCONT.
  Fractional values (0.5s, 1.5m) should be supported via double parsing.

Implementation steps:
1. Create cmd_sleep.c with a parse_duration(const char *arg) helper that
   returns the total seconds as a double.
2. Convert to struct timespec and call nanosleep() with EINTR retry.
3. Add to Makefile SRCS and register in aishell_main.c.
4. Build and verify zero warnings.
5. Test (inside ./aishell):
     sleep 1 && echo "done"
     sleep 0.5
     sleep 1m    # should wait 60 seconds — verify with time
     sleep 1s 2s # sum = 3 seconds
     sleep --help-json
```

---

### Step 17 — `which` command

```
We are working in /home/jagan/aishell/week3 on the AiShell project.
Every command follows the APPANATOMY pattern in CLAUDE.md.
We already have cmd_type.c which identifies how a name would be interpreted
by aishell (built-in, alias, or external). The new which is different: it
only reports the full path of external executables found in PATH.

Task: implement the which built-in command.

Flags to support:
  -a / --all    print all matching paths in PATH (not just the first)
  NAME...       one or more command names to look up

Behaviour:
  Split $PATH on ':'. For each directory, check if DIR/NAME exists and is
  executable (access(path, X_OK)). Print the first match (or all with -a).
  Exit code: 0 if all names were found, 1 if any was not found.
  Do NOT search aishell's built-in registry — only the filesystem PATH.

Implementation steps:
1. Create cmd_which.c.
2. Add to Makefile SRCS and register in aishell_main.c.
3. Build and verify zero warnings.
4. Test (inside ./aishell):
     which ls
     which gcc make python3
     which -a python3
     which nonexistent_command
     echo $?        # should print 1
     which --help-json
```

---

## PHASE 4 — Week 10 Grammar (Loops)

---

### Step 18 — `for` and `while` loops in grammar

```
We are working in /home/jagan/aishell/week3 on the AiShell project.
The BNFC grammar is at bnfc/Grammar.cf.
The AST executor is the switch/dispatch in aishell_main.c (bnfc_repl section).

Task: add for and while loop constructs to the grammar.

PART A — Grammar.cf additions

Add these rules inside the Job section (after IfStmt):

  ForStmt.   Job ::= "for" Word "in" [Word] "do" [Job] "done" ;
  WhileStmt. Job ::= "while" Condition "do" [Job] "done" ;
  UntilStmt. Job ::= "until" Condition "do" [Job] "done" ;
  BreakStmt. Job ::= "break" ;
  ContStmt.  Job ::= "continue" ;

PART B — AST executor (aishell_main.c)

For ForStmt:
  1. Get the word list (the "in" values).
  2. For each value, set the loop variable in the environment (setenv).
  3. Execute the [Job] body.
  4. Unset the variable after the loop completes.

For WhileStmt:
  1. Evaluate the Condition.
  2. While exit code == 0, execute the [Job] body then re-evaluate.

For UntilStmt: same as While but loop while exit code != 0.

BreakStmt / ContStmt: use a loop_depth counter (static int) and a
  break_flag / continue_flag to short-circuit the body execution.

PART C — Multiline input support in bnfc_repl()

For loops require multiple lines of input. Detect open constructs:
  After parsing, if the token stream ends with "do" or "then" without
  a matching "done"/"fi", keep reading with a "> " continuation prompt
  until the construct is complete, then parse and execute the full buffer.

PART D — Regenerate and rebuild
  cd bnfc && bnfc --c -m Grammar.cf && cd .. && make clean && make

PART E — Test (inside ./aishell):
  for f in one two three; do echo "item: $f"; done
  for i in 1 2 3 4 5; do echo $i; done
  while false; do echo "never"; done
  i=0; while test $i -lt 3; do echo $i; i=$((i+1)); done
```

---

## PHASE 5 — Week 11 Commands (Shell Scripting Primitives)

---

### Step 19 — `true` and `false` commands

```
We are working in /home/jagan/aishell/week3 on the AiShell project.
Every command follows the APPANATOMY pattern in CLAUDE.md.

Task: implement both true and false as built-in commands.
These are the simplest possible commands — they exist purely for their exit codes.

true:  always exits with code 0 (success).
false: always exits with code 1 (failure).

They are essential for loop conditions:
  while true; do read line; echo $line; done

Implement both in a single file cmd_true_false.c with two cmd_spec_t structs:
  cmd_true_spec  — name="true",  run returns 0
  cmd_false_spec — name="false", run returns 1

Both register functions (register_true_command, register_false_command)
live in the same file.

Both support --help and --help-json but take no other arguments.

Steps:
1. Create cmd_true_false.c.
2. Add to Makefile SRCS.
3. Add both extern declarations and register calls in aishell_main.c.
4. Build and verify zero warnings.
5. Test (inside ./aishell):
     true && echo "yes"
     false || echo "fallback"
     if true; then echo "branch taken"; fi
     true --help-json
     false --help-json
```

---

### Step 20 — `file` command

```
We are working in /home/jagan/aishell/week3 on the AiShell project.
Every command follows the APPANATOMY pattern in CLAUDE.md.

Task: implement the file built-in command that detects file types.

Do NOT link against libmagic. Implement file type detection by:
1. Checking if the path is a directory, symlink, block/char device, FIFO,
   or socket using stat()/S_IS* macros.
2. For regular files, reading the first 16 bytes (magic bytes) and checking:
   - ELF:         starts with \x7fELF
   - PNG:         starts with \x89PNG
   - JPEG:        starts with \xff\xd8\xff
   - PDF:         starts with %PDF
   - ZIP/JAR/DOCX: starts with PK\x03\x04
   - gzip:        starts with \x1f\x8b
   - tar:         bytes 257-262 == "ustar"
   - Shell script: starts with #! (shebang) — read first line to show interpreter
   - UTF-8 text:  all bytes in range 0x09-0x0D or 0x20-0x7E or valid UTF-8 multi-byte
   - Binary:      anything else

Flags:
  -b / --brief     do not prepend filename to output
  FILE...          one or more paths to inspect

Steps:
1. Create cmd_file.c.
2. Add to Makefile SRCS and register in aishell_main.c.
3. Build and verify zero warnings.
4. Test (inside ./aishell):
     file aishell
     file aishell_main.c
     file commands.json
     file /dev/null
     file /tmp
     file --help-json
```

---

### Step 21 — `md5sum` and `sha256sum` commands

```
We are working in /home/jagan/aishell/week3 on the AiShell project.
Every command follows the APPANATOMY pattern in CLAUDE.md.

Task: implement md5sum and sha256sum as built-in commands.
Implement both algorithms from scratch in C (no external libraries).

md5sum: RFC 1321 MD5 (128-bit hash, 32 hex chars)
sha256sum: FIPS 180-4 SHA-256 (256-bit hash, 64 hex chars)

Flags (same for both):
  -c / --check    read checksums from FILE and verify them
  -b / --binary   read in binary mode (ignored on Linux — same as text)
  FILE...         files to hash; read stdin if no files given

Output format (matching GNU coreutils):
  <hexdigest>  <filename>
  (two spaces — binary mode marker would be *, text is space)

For -c mode: read lines of the above format, recompute the hash, print
  "FILENAME: OK" or "FILENAME: FAILED", exit 1 if any failed.

Implementation:
  Put the MD5 and SHA-256 implementations in a shared hash_utils.c / hash_utils.h.
  Both cmd_md5sum.c and cmd_sha256sum.c include hash_utils.h.

Steps:
1. Create hash_utils.h and hash_utils.c with md5_file() and sha256_file()
   functions that take a FILE* and return a hex string.
2. Create cmd_md5sum.c and cmd_sha256sum.c.
3. Add all three .c files to Makefile SRCS.
4. Register both commands in aishell_main.c.
5. Build and verify zero warnings.
6. Test (inside ./aishell):
     echo "hello" | md5sum
     md5sum aishell_main.c
     sha256sum aishell_main.c
     md5sum aishell_main.c > /tmp/check.md5
     md5sum -c /tmp/check.md5
     md5sum --help-json
     sha256sum --help-json
```

---

### Step 22 — `alias` command + session alias table

```
We are working in /home/jagan/aishell/week3 on the AiShell project.

Task: implement session aliases — a persistent in-process substitution table.

PART A — Alias storage (aishell_main.c)

Add to aishell_main.c (in the REPL state section, not inside any function):

  #define ALIAS_MAX 64
  typedef struct { char name[64]; char value[256]; } Alias;
  static Alias alias_table[ALIAS_MAX];
  static int   alias_count = 0;

Add three helpers:
  alias_set(name, value)    — add or update an entry
  alias_get(name)           — return value or NULL
  alias_del(name)           — remove an entry

PART B — Expansion before parsing (bnfc_repl)

Before passing the input line to the BNFC parser, check if the first
token (up to the first space) matches an alias name. If so, replace
that token with the alias value. This covers the common case.
Do NOT expand recursively to avoid infinite loops.

PART C — cmd_alias.c command

Flags:
  alias              (no args) — print all defined aliases as "alias name='value'"
  alias NAME=VALUE   — define an alias
  alias NAME         — print the alias definition for NAME
  unalias NAME       — remove an alias (implement in cmd_alias.c as a subcommand
                       or handle in cmd_unset.c with an --alias flag)

The cmd_alias_run() function directly manipulates the alias_table[] via
the helpers from PART A (declare them extern in cmd_alias.c).

Steps:
1. Add alias table and helpers to aishell_main.c.
2. Add expansion step in bnfc_repl() before the BNFC parse call.
3. Create cmd_alias.c.
4. Add to Makefile SRCS and register in aishell_main.c.
5. Build and verify zero warnings.
6. Test (inside ./aishell):
     alias ll='ls -la'
     ll
     alias
     alias ll
     alias grep='grep --color=auto'
     grep TODO aishell_main.c
     alias --help-json
```

---

### Step 23 — Command history + arrow-key navigation

```
We are working in /home/jagan/aishell/week3 on the AiShell project.

Task: add command history with ↑/↓ navigation to the aishell REPL.

Two options — choose Option A:

OPTION A (recommended): link against readline
  1. Check if readline is available:
     apt list --installed 2>/dev/null | grep libreadline-dev
     If missing: sudo apt install libreadline-dev
  2. In Makefile add -lreadline to LDFLAGS.
  3. In aishell_main.c add:
       #include <readline/readline.h>
       #include <readline/history.h>
  4. In bnfc_repl(), replace the getline() call with:
       char *line = readline(display_prompt);
       if (!line) break;  // EOF
       if (line[0] != '\0') add_history(line);
       // process line
       free(line);
  5. At shell startup call:
       using_history();
       read_history(history_file_path);   // ~/.aishell_history
  6. At shell exit call:
       write_history(history_file_path);

OPTION B (no dependency): circular buffer
  Maintain a static char history[100][1024] ring buffer in aishell_main.c.
  Use raw terminal mode (tcsetattr TCSANOW with ~ICANON ~ECHO) to read
  escape sequences (\x1b[A = up, \x1b[B = down) and rewrite the current
  line on the terminal.

PART B — history command

Implement cmd_history.c:
  Flags:
    -c / --clear    clear the history list
    -n N            show only the last N entries
    (no args)       show all history with line numbers

Steps:
1. Implement OPTION A (readline).
2. Create cmd_history.c that calls history_list() from readline's API to
   print entries, and clear_history() for -c.
3. Add -lreadline to LDFLAGS in Makefile.
4. Add cmd_history.c to SRCS and register in aishell_main.c.
5. Build and verify zero warnings.
6. Test:
   - Run ./aishell and type several commands
   - Press ↑ — should recall previous command
   - Press ↑ again — should go further back
   - type: history
   - type: history -n 5
   - Exit and re-enter aishell — history should persist
```

---

### Step 24 — Tab completion

```
We are working in /home/jagan/aishell/week3 on the AiShell project.
Step 23 must be complete (readline integrated) before this step.

Task: add tab completion for built-in command names and file paths.

PART A — Command name completion

When the user presses TAB at the start of input (or after a pipe/semicolon),
complete from the list of registered commands.

Using readline's completion API:
  1. Write a generator function:
       static char *command_generator(const char *text, int state) {
           static int idx;
           if (!state) idx = 0;
           while (idx < registered_command_count) {
               const char *name = get_command_name(idx++);
               if (strncmp(name, text, strlen(text)) == 0)
                   return strdup(name);
           }
           return NULL;
       }
  2. Write a completer:
       static char **aishell_completer(const char *text, int start, int end) {
           if (start == 0)
               return rl_completion_matches(text, command_generator);
           return NULL;  // fall through to readline's filename completion
       }
  3. Set: rl_attempted_completion_function = aishell_completer;

PART B — File path completion

When TAB is pressed mid-argument (start > 0), readline's default filename
completion (rl_filename_completion_function) handles it automatically once
we return NULL from the completer. No extra code needed.

PART C — @ query completion (bonus)

When the current line starts with "@", complete from the alias/keyword
list in commands.json.
  - Load keywords from g_registry[] at startup into a static array.
  - In the completer, detect the "@" prefix and switch to the keyword generator.

Steps:
1. Add the generator and completer functions to aishell_main.c.
2. Register: rl_attempted_completion_function = aishell_completer;
   in the bnfc_repl() initialization section.
3. Build and verify zero warnings.
4. Test:
   - Type "ca" + TAB — should complete to "cat"
   - Type "ls /et" + TAB — should complete the path
   - Type "@ disk" + TAB — should offer "disk usage", "disk space" etc.
```

---

## PHASE 6 — Week 12 Commands (Networking + Scripting)

---

### Step 25 — `ping` command

```
We are working in /home/jagan/aishell/week3 on the AiShell project.
Every command follows the APPANATOMY pattern in CLAUDE.md.

Task: implement the ping built-in command using raw ICMP sockets.

Flags to support:
  -c N / --count=N       stop after sending N packets (default 4)
  -W N / --timeout=N     wait N seconds for each reply (default 1)
  -i N / --interval=N    wait N seconds between packets (default 1)
  -q / --quiet           only print summary, not per-packet output
  HOST                   hostname or IP address to ping

Behaviour:
  Use a raw socket (SOCK_RAW, IPPROTO_ICMP) and send ICMP ECHO_REQUEST
  packets. Receive ICMP ECHO_REPLY and measure round-trip time.
  Print: PING host (ip): 56 bytes of data.
         64 bytes from ip: icmp_seq=1 ttl=64 time=0.456 ms
  Print summary: N packets transmitted, N received, N% loss, time Nms
  Requires CAP_NET_RAW (running as root or with setcap).
  If raw socket creation fails with EPERM, print a hint and fall back to
  shelling out to /bin/ping if available.

Steps:
1. Create cmd_ping.c with an icmp_checksum() helper.
2. Add to Makefile SRCS and register in aishell_main.c.
3. Build and verify zero warnings.
4. Test (as root inside ./aishell):
     ping -c 3 127.0.0.1
     ping -c 2 -W 2 google.com
     ping -q -c 5 localhost
     ping --help-json
```

---

### Step 26 — `nc` (netcat) command

```
We are working in /home/jagan/aishell/week3 on the AiShell project.
Every command follows the APPANATOMY pattern in CLAUDE.md.

Task: implement a basic nc (netcat) built-in for TCP connections.

Flags to support:
  -l / --listen          listen mode: accept an incoming connection on PORT
  -p PORT / --port=PORT  port to listen on (with -l) or connect to (positional)
  -z / --zero-io         port scan mode: connect and immediately close
  -v / --verbose         print connection status messages
  -w N / --wait=N        timeout in seconds (default: none)
  HOST PORT              connect to HOST on PORT (client mode, no -l)

Behaviour:
  Client mode (no -l): connect via TCP, then relay stdin→socket and socket→stdout
    using select() in a loop. This enables:
      echo "GET / HTTP/1.0\r\n" | nc example.com 80
  Listen mode (-l): bind, listen, accept one connection, then relay.
  Zero-io mode (-z): connect and report open/closed without sending data.
    nc -z hostname 22 && echo "SSH port open"

Steps:
1. Create cmd_nc.c. Use select() with two fds (stdin and the socket)
   for the relay loop. Use SO_REUSEADDR for listen mode.
2. Add to Makefile SRCS and register in aishell_main.c.
3. Build and verify zero warnings.
4. Test (inside ./aishell — in two terminals):
   Terminal 1: nc -l -p 9999
   Terminal 2: echo "hello" | nc localhost 9999
   # Then test port scan:
   nc -z localhost 22 && echo "port 22 open"
   nc -z localhost 9999 || echo "port 9999 closed"
   nc --help-json
```

---

### Step 27 — `xargs` command

```
We are working in /home/jagan/aishell/week3 on the AiShell project.
Every command follows the APPANATOMY pattern in CLAUDE.md.

Task: implement the xargs built-in command.

Flags to support:
  -I REPLACE / --replace=REPLACE   replace REPLACE string in CMD with input item
                                   (e.g. -I {} means use {} as placeholder)
  -n N / --max-args=N              use at most N arguments per command line
  -P N / --max-procs=N             run up to N processes in parallel
  -d DELIM / --delimiter=DELIM     use DELIM as input item delimiter (default: whitespace)
  -0 / --null                      input items are null-terminated (for find -print0)
  CMD [ARGS...]                    command to run (default: echo)

Behaviour:
  Read items from stdin (split by whitespace or -d DELIM or \0 with -0).
  Build command lines from CMD + items, respecting the -n limit.
  With -I {}: for each item, run CMD with {} replaced by the item.
  With -P N: use fork() to run N commands simultaneously, wait() for them.
  Exit code: 0 if all commands succeed, 1 if any fails, 123 if any exits
  with code 255, 124 on timeout, 125 if xargs itself fails.

Steps:
1. Create cmd_xargs.c. Use a dynamic argv builder.
   For -P > 1: maintain a pid_t children[N] array, use waitpid(WNOHANG)
   to reap finished children and fork new ones.
2. Add to Makefile SRCS and register in aishell_main.c.
3. Build and verify zero warnings.
4. Test (inside ./aishell):
     echo "one two three" | xargs echo "item:"
     find . -name "*.c" | xargs wc -l
     find . -name "*.c" -print0 | xargs -0 grep -l "TODO"
     echo "a b c" | xargs -n 1 echo
     echo "a b c" | xargs -I{} echo "value={}"
     xargs --help-json
```

---

### Step 28 — `read` command

```
We are working in /home/jagan/aishell/week3 on the AiShell project.
Every command follows the APPANATOMY pattern in CLAUDE.md.

Task: implement the read built-in command (reads one line from stdin
into a shell variable).

Flags to support:
  -p PROMPT / --prompt=PROMPT   print PROMPT before reading (to stderr)
  -s / --silent                 do not echo input (for passwords)
  -n N / --nchars=N             return after reading N characters
  -d DELIM / --delimiter=DELIM  stop at DELIM instead of newline
  -t N / --timeout=N            timeout after N seconds (exit 1 on timeout)
  VAR                           variable name to assign (required)

Behaviour:
  Read one line from stdin. Strip the trailing delimiter.
  Set the variable in the current shell environment with setenv(VAR, value, 1).
  For -s: use tcsetattr() to turn off ECHO before reading, restore after.
  For -t N: use select() with a timeout before calling read().
  For -n N: stop reading after N chars even if no delimiter seen.

Steps:
1. Create cmd_read.c.
2. Add to Makefile SRCS and register in aishell_main.c.
3. Build and verify zero warnings.
4. Test (inside ./aishell):
     read NAME && echo "Hello $NAME"
     read -p "Enter password: " -s PASS && echo "" && echo "got it"
     read -t 3 ANSWER || echo "timed out"
     read -n 1 KEY && echo "key: $KEY"
     read --help-json
```

---

### Step 29 — `test` / `[` command

```
We are working in /home/jagan/aishell/week3 on the AiShell project.
Every command follows the APPANATOMY pattern in CLAUDE.md.

Task: implement the test built-in (also available as [ ... ]).
This is essential for if/while scripting.

Operators to support:

  File tests:
    -f FILE    regular file exists
    -d FILE    directory exists
    -e FILE    any file exists
    -r FILE    file is readable
    -w FILE    file is writable
    -x FILE    file is executable
    -s FILE    file size > 0
    -L FILE    path is a symbolic link
    -z STR     string is empty
    -n STR     string is non-empty

  String comparisons:
    STR1 = STR2     strings are equal
    STR1 != STR2    strings differ

  Numeric comparisons:
    N1 -eq N2   equal
    N1 -ne N2   not equal
    N1 -lt N2   less than
    N1 -le N2   less than or equal
    N1 -gt N2   greater than
    N1 -ge N2   greater than or equal

  Logical:
    EXPR -a EXPR    AND (both true)
    EXPR -o EXPR    OR (either true)
    ! EXPR          NOT

  [ form: the last argument must be ] — strip it before processing.

Behaviour:
  Exit 0 if the condition is true, 1 if false, 2 on invalid usage.
  Register the command twice: once as "test", once as "[".

Steps:
1. Create cmd_test.c. Parse argv[] iteratively. Implement the [ alias
   by registering the same run function under a second cmd_spec_t
   (cmd_bracket_spec with name "[").
2. Add to Makefile SRCS and register both "test" and "[" in aishell_main.c.
3. Build and verify zero warnings.
4. Test (inside ./aishell):
     test -f aishell_main.c && echo "exists"
     test -d /tmp && echo "is dir"
     test "hello" = "hello" && echo "equal"
     test 5 -gt 3 && echo "5 > 3"
     [ -x ./aishell ] && echo "executable"
     if test -f commands.json; then echo "found"; fi
     test --help-json
```

---

## PHASE 7 — Week 12 Advanced Grammar

---

### Step 30 — Arithmetic expansion `$(( ))`

```
We are working in /home/jagan/aishell/week3 on the AiShell project.
The BNFC grammar is at bnfc/Grammar.cf.

Task: add arithmetic expansion $(( expr )) so expressions like
echo $((2 + 3 * 4)) and sleep $((60 * 5)) work.

PART A — Grammar token

Add to Grammar.cf:
  token ArithExp ( '$' '(' '(' ( char - [")"] )* ')' ')' ) ;

Add ArithExp as a valid Arg type alongside Word and CmdSubst
(if Step 3 was implemented):
  ArithArg. Arg ::= ArithExp ;

PART B — AST executor expansion

When building argv[] from Arg nodes, detect ArithArg:
1. Strip the outer $((  )) wrapper.
2. Evaluate the arithmetic expression using a simple recursive descent
   parser that handles: + - * / % ( ) unary minus and integer operands.
3. Substitute the result (as a decimal string) as the argument value.

The arithmetic evaluator needs these precedence levels:
  Level 1 (lowest):  +  -   (additive)
  Level 2:           *  /  %  (multiplicative)
  Level 3 (highest): unary - , parenthesised ( expr )
  Terminals: decimal integer literals

PART C — Rebuild and test

cd bnfc && bnfc --c -m Grammar.cf && cd .. && make clean && make
Test inside ./aishell:
  echo $((2 + 3))
  echo $((10 * 5 - 3))
  echo $((100 / 4))
  sleep $((1 + 1))
  x=5; echo $((x * 2))     # variable reference in arithmetic
```

---

### Step 31 — Process substitution `<(...)` and `>(...)`

```
We are working in /home/jagan/aishell/week3 on the AiShell project.
The BNFC grammar is at bnfc/Grammar.cf.
Step 3 (command substitution) must be complete before this step.

Task: add process substitution so expressions like
  diff <(sort file1) <(sort file2)
  tee >(gzip > out.gz)
work in the shell.

PART A — Grammar tokens

Add to Grammar.cf:
  token ProcSubstIn  ( '<' '(' ( char - [")"] )* ')' ) ;
  token ProcSubstOut ( '>' '(' ( char - [")"] )* ')' ) ;

Add as Arg types:
  PSubstInArg.  Arg ::= ProcSubstIn ;
  PSubstOutArg. Arg ::= ProcSubstOut ;

PART B — AST executor

When a PSubstInArg is encountered:
1. Create a pipe: int pfd[2]; pipe(pfd);
2. Fork a child process:
   - Child: close pfd[0]; dup2(pfd[1], STDOUT_FILENO); exec the inner command
   - Parent: close pfd[1]; the argument becomes /proc/self/fd/N (pfd[0])
3. Pass /proc/self/fd/N as the actual file path to the outer command.

For PSubstOutArg (>(...)):
Same but reverse — the argument becomes a write-end fd path.

This approach uses /proc/self/fd/ which is Linux-specific and always
available on WSL2.

PART C — Rebuild and test
cd bnfc && bnfc --c -m Grammar.cf && cd .. && make clean && make
Test inside ./aishell:
  diff <(ls /tmp) <(ls /var)
  diff <(sort /etc/passwd) <(sort /etc/passwd)   # should show no diff
  cat /etc/passwd | tee >(wc -l) >(grep root) > /dev/null
```

---

## PHASE 8 — Final Regression + Commit

---

### Step 32 — Full regression check and commit

```
We are working in /home/jagan/aishell/week3 on the AiShell project.

We have completed Phases 1–7 (grammar hardening, 14 new commands,
readline, tab completion, loops, arithmetic, process substitution).

Task: run the full regression suite, fix any failures, then commit.

1. Clean build:
   make clean && make
   Confirm: zero warnings.

2. Run all test suites:
   bash test_week8.sh
   bash test_week7.sh
   bash test_week6.sh
   bash test_week5.sh
   Report total pass/fail for each.

3. Quick smoke test of every new command (inside ./aishell):
   uniq / cut / tr / grep / diff / tee
   du / df / ln / chmod / chown / sleep / which
   true / false / file / md5sum / sha256sum
   alias / history / read / test / ping / nc / xargs

4. Grammar smoke test (inside ./aishell):
   ps -eo pid,%cpu,%mem | head -5           # Word charset fix
   ls /nonexistent 2>/dev/null              # stderr redirect
   echo $((6 * 7))                         # arithmetic
   for i in 1 2 3; do echo $i; done        # for loop
   diff <(echo hello) <(echo world)        # process substitution

5. Update CLAUDE.md:
   - Update the command count (was 32, now 32 + new commands)
   - Add entries to the Commands table for every new command
   - Update the Grammar section to list newly supported syntax

6. Commit:
   git add -A
   git commit -m "Week 9-12: 14 new commands, grammar hardening, readline"
```

---

## Quick Reference — Adding a New Command Checklist

```
For any new command cmd_<name>.c:

[ ] 1. Create cmd_<name>.c
       - build_<name>_argtable() with all flags + --help + --help-json
       - <name>_print_usage(FILE *fp)
       - cmd_<name>_run(int argc, char **argv)
       - cmd_spec_t cmd_<name>_spec = { .name=..., .summary=..., .run=..., .print_usage=... }
       - void register_<name>_command(void)

[ ] 2. Makefile — add cmd_<name>.c to SRCS

[ ] 3. aishell_main.c — add:
       extern void register_<name>_command(void);   (near top with other externs)
       register_<name>_command();                   (inside register_all_commands)

[ ] 4. make clean && make   — zero warnings required

[ ] 5. Test:
       ./aishell <name> --help
       ./aishell <name> --help-json
       <functional tests>

[ ] 6. Run full regression:
       bash test_week8.sh && bash test_week7.sh && bash test_week6.sh && bash test_week5.sh
```

---

## Quick Reference — Grammar Change Checklist

```
For any Grammar.cf change:

[ ] 1. Edit bnfc/Grammar.cf

[ ] 2. cd bnfc && bnfc --c -m Grammar.cf
       (regenerates Absyn.c/h, Lexer.l, Parser.y, Printer.c/h)

[ ] 3. cd .. && make clean && make
       (recompiles everything including BNFC-generated files)

[ ] 4. If the change adds a new AST node:
       - Add a case to the executor switch in aishell_main.c
       - Add a case to the printer in bnfc/Printer.c if needed

[ ] 5. Test new syntax inside ./aishell

[ ] 6. Run full regression suites
```
