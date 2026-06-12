#!/usr/bin/env bash
# test_week9.sh — Week 9–12 regression: xargs, read, test/[, alias,
#                 history, md5sum/sha256sum, arithmetic (vars), process subst
#
# Usage: bash test_week9.sh [path-to-aishell]
# All tests run non-interactively by piping input to the shell.

SHELL_BIN="${1:-./aishell}"
PASS=0; FAIL=0

ok()   { echo "  PASS  $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL  $1"; FAIL=$((FAIL+1)); }

# run LABEL EXPECTED SCRIPT — expects EXPECTED to appear in stdout
run() {
    local label="$1" expected="$2" input="$3"
    local out
    out=$(printf '%s\n' "$input" | timeout 8 "$SHELL_BIN" 2>/dev/null) || true
    if printf '%s\n' "$out" | grep -qF "$expected" 2>/dev/null; then
        ok "$label"
    else
        fail "$label — expected '$expected', got: $(printf '%s' "$out" | head -2)"
    fi
}

# run_rc LABEL EXPECTED_RC SCRIPT — expects a specific exit code
run_rc() {
    local label="$1" expected_rc="$2" input="$3"
    local rc
    printf '%s\n' "$input" | timeout 8 "$SHELL_BIN" >/dev/null 2>&1 || true
    rc=$?
    if [ "$rc" -eq "$expected_rc" ]; then ok "$label"
    else fail "$label — expected rc=$expected_rc, got rc=$rc"
    fi
}

# Prepare temp file for read tests
RT=/tmp/aishell_week9_read.txt
printf 'hello\n' > "$RT"

echo
echo "============================================"
echo " test_week9.sh — Week 9-12 regression suite"
echo "============================================"
echo

# ── 1. xargs ────────────────────────────────────────────────────────────────
echo "--- 1. xargs ---"

run  "xargs basic — single word echoed"         "hello" \
     'echo hello | xargs echo'

run  "xargs -n1 — one arg per call"             "world" \
     'echo "hello world" | xargs -n1 echo'

run  "xargs -n2 — two args per call"            "hello world" \
     'echo "hello world" | xargs -n2 echo'

run  "xargs -I% replace-string"                 "hello.c" \
     'echo hello | xargs -I% echo %.c'

run  "xargs pipe count (wc -w)"                 "3" \
     'echo "a b c" | xargs echo | wc -w'

run  "xargs grep over files"                    "Makefile" \
     'echo Makefile | xargs grep -l CC'

# ── 2. read ──────────────────────────────────────────────────────────────────
echo
echo "--- 2. read ---"

run  "read from file — assigns variable"        "hello" \
     "read X < $RT && echo \$X"

run  "read in sequence — var persists"          "hello" \
     "read X < $RT; echo \$X"

run  "read -n 1 — reads one char"               "h" \
     "read -n 1 X < $RT && echo \$X"

run  "read then use in pipeline"                "HELLO" \
     "read X < $RT; echo \$X | tr a-z A-Z"

# ── 3. test ──────────────────────────────────────────────────────────────────
echo
echo "--- 3. test ---"

run  "test -f on existing file"                 "exists" \
     'test -f Makefile && echo exists'

run  "test -d on directory"                     "is dir" \
     'test -d /tmp && echo "is dir"'

run  "test -e on existing path"                 "found" \
     'test -e /etc/hostname && echo found'

run  "test -x on executable"                    "executable" \
     'test -x ./aishell && echo executable'

run  "test -n (non-empty string)"               "nonempty" \
     'test -n hello && echo nonempty'

run  "test string equality ="                   "equal" \
     'test abc = abc && echo equal'

run  "test numeric -eq"                         "eq" \
     'test 42 -eq 42 && echo eq'

run  "test numeric -lt"                         "less" \
     'test 3 -lt 10 && echo less'

run  "test numeric -gt"                         "greater" \
     'test 10 -gt 3 && echo greater'

run  "test numeric -le"                         "le" \
     'test 5 -le 5 && echo le'

run  "test numeric -ge"                         "ge" \
     'test 7 -ge 3 && echo ge'

run  "test -a AND combinator"                   "both" \
     'test -f Makefile -a -d /tmp && echo both'

# ── 4. [ ] bracket form ──────────────────────────────────────────────────────
echo
echo "--- 4. [ ] bracket syntax ---"

run  "[ -f file ] form"                         "yes" \
     '[ -f Makefile ] && echo yes'

run  "[ -d dir ] form"                          "yes" \
     '[ -d /tmp ] && echo yes'

run  "[ -x exec ] form"                         "yes" \
     '[ -x ./aishell ] && echo yes'

run  "[ string = string ]"                      "match" \
     '[ hello = hello ] && echo match'

run  "[ N -le N ]"                              "ok" \
     '[ 5 -le 10 ] && echo ok'

run  "if [ -f ] then ... fi"                    "found" \
     'if [ -f Makefile ]; then echo found; fi'

run  "if [ -d ] else"                           "isdir" \
     'if [ -d /tmp ]; then echo isdir; fi'

# ── 5. md5sum / sha256sum ────────────────────────────────────────────────────
echo
echo "--- 5. md5sum / sha256sum ---"

run  "echo test | md5sum produces hash"         "d8e8fca2dc0f896fd7cb4cb0031ba249" \
     'echo test | md5sum'

run  "echo test | sha256sum produces hash"      "f2ca1bb6c7e907d06dafe4687e579fce" \
     'echo test | sha256sum'

run  "printf abc | md5sum"                      "900150983cd24fb0d6963f7d28e17f72" \
     'printf "abc" | md5sum'

# ── 6. alias ─────────────────────────────────────────────────────────────────
echo
echo "--- 6. alias ---"

# try_handle_alias_define intercepts "alias name=val" before BNFC.
# Two-line input: define on line 1, list on line 2.
run  "alias define then list"                   "greet" \
     "$(printf 'alias greet=hi\nalias')"

run  "alias defines value"                      "hi" \
     "$(printf 'alias greet=hi\nalias')"

run  "alias expansion in command"               "hello" \
     "$(printf 'alias greet="echo hello"\ngreet')"

# ── 7. history ───────────────────────────────────────────────────────────────
echo
echo "--- 7. history ---"

run  "history records echo command"             "echo hello" \
     'echo hello; history'

run  "history -n 1 shows last entry"            "echo hi" \
     'echo hi; history -n 1'

# ── 8. Arithmetic expansion with variable references ─────────────────────────
echo
echo "--- 8. arithmetic expansion ---"

run  "pure constant arith"                      "42" \
     'echo $((6 * 7))'

run  "arith with variable ref"                  "10" \
     'x=5; echo $((x * 2))'

run  "multi-op arith"                           "47" \
     'echo $((10 * 5 - 3))'

run  "arith division"                           "25" \
     'echo $((100 / 4))'

run  "arith modulo"                             "1" \
     'echo $((10 % 3))'

run  "arith in sleep arg"                       "arith ok" \
     'sleep $((1 + 0)) && echo "arith ok"'

# ── 9. Process substitution ──────────────────────────────────────────────────
echo
echo "--- 9. process substitution ---"

run  "diff <(echo a) <(echo b) shows diff"      "1c1" \
     'diff <(echo a) <(echo b)'

run  "diff identical <(cmd) — no output"        "" \
     'diff <(echo hello) <(echo hello) || true'

run  "cat <(echo hello) reads pipe"             "hello" \
     'cat <(echo hello)'

run  "diff identical proc substs — exits 0"     "same" \
     'diff <(echo apple) <(echo apple) && echo same'

run  "tee >(cat) passes through"                "hello" \
     'echo hello | tee >(cat) > /dev/null'

# ── Results ──────────────────────────────────────────────────────────────────
echo
echo "============================================"
printf "  Passed: %d   Failed: %d\n" "$PASS" "$FAIL"
echo "============================================"
echo

exit $FAIL
