#!/usr/bin/env bash
# test_week6.sh — Week 6 pthread integration tests
# Each test sends commands to ./aishell via a pipe (2>/dev/null suppresses the
# prompt which goes to stderr in non-tty mode).  We capture stdout, grep for
# expected output, and report PASS / FAIL.
#
# Run:  bash test_week6.sh
# All tests should show PASS on a correct build.

set -uo pipefail

SHELL_BIN="./aishell"
PASS=0
FAIL=0

# ── helper ────────────────────────────────────────────────────────────────────
check() {
    local desc="$1" expected="$2" actual="$3"
    if echo "$actual" | grep -qF "$expected"; then
        printf "  PASS  %s\n" "$desc"
        ((PASS++))
    else
        printf "  FAIL  %s\n" "$desc"
        printf "        expected substring: %s\n" "$expected"
        printf "        actual output:      %s\n" "$actual"
        ((FAIL++))
    fi
}

check_absent() {
    local desc="$1" absent="$2" actual="$3"
    if echo "$actual" | grep -qF "$absent"; then
        printf "  FAIL  %s  (found unexpected: %s)\n" "$desc" "$absent"
        ((FAIL++))
    else
        printf "  PASS  %s\n" "$desc"
        ((PASS++))
    fi
}

check_exit() {
    local desc="$1" expected_code="$2" actual_code="$3"
    if [[ "$actual_code" == "$expected_code" ]]; then
        printf "  PASS  %s\n" "$desc"
        ((PASS++))
    else
        printf "  FAIL  %s  (exit %s, wanted %s)\n" \
               "$desc" "$actual_code" "$expected_code"
        ((FAIL++))
    fi
}

# Times a command with a deadline; kills it and reports FAIL if it hangs.
# Usage: timed_run SECONDS "description" <shell_input>
timed_run() {
    local timeout_s="$1" desc="$2" input="$3"
    local out
    out=$(echo "$input" | timeout "$timeout_s" "$SHELL_BIN" 2>/dev/null)
    local rc=$?
    if [[ $rc -eq 124 ]]; then
        printf "  FAIL  %s  (HUNG — killed after %ss)\n" "$desc" "$timeout_s"
        ((FAIL++))
        echo ""
        return
    fi
    echo "$out"
}

echo "=================================================================="
echo " Week 6 — pthread integration test suite"
echo "=================================================================="

# ── 1. Basic built-in runs in a thread ────────────────────────────────────────
echo ""
echo "1. Basic built-in runs in a thread"

out=$(printf 'help\n' | "$SHELL_BIN" 2>/dev/null)
check "help prints command list"        "aishell — available built-in commands" "$out"
check "help output includes 'exit'"     "exit"                                  "$out"
check "help output includes 'jobs'"     "jobs"                                  "$out"

# ── 2. Built-in piped to external  (THREAD → FORK) ────────────────────────────
echo ""
echo "2. Built-in | external  (THREAD → FORK)"

out=$(timed_run 5 "help | grep exit" 'help | /usr/bin/grep exit')
check "help | grep exit — line found"   "exit"   "$out"
check_absent "no extra lines leaked"    "jobs"   "$out"

out=$(timed_run 5 "help | wc -l counts lines" 'help | /usr/bin/wc -l')
# help prints ≥7 lines; just verify we got a non-zero number
lines=$(echo "$out" | tr -d ' ')
if [[ "$lines" -gt 0 ]] 2>/dev/null; then
    printf "  PASS  help | wc -l reports %s lines\n" "$lines"
    ((PASS++))
else
    printf "  FAIL  help | wc -l — unexpected output: %s\n" "$out"
    ((FAIL++))
fi

# ── 3. External piped to built-in  (FORK → THREAD) ────────────────────────────
echo ""
echo "3. External | built-in  (FORK → THREAD)"

# 'jobs' ignores stdin but the plumbing must not deadlock.
out=$(timed_run 5 "external | jobs does not hang" '/usr/bin/echo hello | jobs')
check_exit "external | jobs exits cleanly (no hang)" "0" "$?"
# No background jobs, so jobs output is empty — verify no FAIL output
check_absent "no error message from jobs" "error" "$out"

# A 2-stage pipeline where the first stage is external and produces data the
# second consumes: verify data flows end-to-end.
out=$(timed_run 5 "echo | wc -c measures bytes" '/usr/bin/echo hello | /usr/bin/wc -c')
check "echo hello | wc -c — byte count correct" "6" "$out"

# ── 4. Background built-in (pthread_detach equivalent — now joinable) ──────────
echo ""
echo "4. Background built-in (&)"

out=$(timed_run 5 "jobs & creates thread job" "$(printf 'help &\n/usr/bin/sleep 0.2\njobs\n')")
check "help & shows [N] (thread)"           "(thread)"      "$out"
check "subsequent jobs shows Thread label"  "Thread"        "$out"

out=$(timed_run 5 "help & runs and can be seen" "$(printf 'jobs &\n/usr/bin/sleep 0.2\njobs\n')")
check "jobs & shows job table entry"        "(thread)"      "$out"

# ── 5. cd guard ───────────────────────────────────────────────────────────────
echo ""
echo "5. cd / exit guards"

out=$(printf 'cd &\n' | "$SHELL_BIN" 2>&1)
check "cd & is rejected"              "cannot be run in background"  "$out"

out=$(printf 'exit &\n' | "$SHELL_BIN" 2>&1)
check "exit & is rejected"            "cannot be run in background"  "$out"

# ── 6. cd changes directory and prompt reflects it ────────────────────────────
echo ""
echo "6. cd changes working directory"

out=$(printf 'cd /tmp\npwd\n' | "$SHELL_BIN" 2>/dev/null)
check "cd /tmp; pwd prints /tmp"      "/tmp"   "$out"

out=$(printf 'cd /tmp\n' | "$SHELL_BIN" 2>&1)
check "prompt shows new dir after cd" "[/tmp]" "$out"

out=$(printf 'cd /nonexistent_dir_xyz\npwd\n' | "$SHELL_BIN" 2>/dev/null)
check_absent "failed cd doesn't change dir" "/nonexistent" "$out"

# ── 7. exit code propagation ──────────────────────────────────────────────────
echo ""
echo "7. exit code propagation"

printf 'exit 0\n' | "$SHELL_BIN" 2>/dev/null; check_exit "exit 0 returns 0" "0" "$?"
printf 'exit 42\n' | "$SHELL_BIN" 2>/dev/null; check_exit "exit 42 returns 42" "42" "$?"
printf 'exit 1\n' | "$SHELL_BIN" 2>/dev/null; check_exit "exit 1 returns 1" "1" "$?"

# ── 8. exit cleans up background threads without hanging ──────────────────────
echo ""
echo "8. exit cleans up threads without hanging"

# Spawn multiple background built-ins, then exit — must finish within 5 s.
out=$(timed_run 5 "exit with 3 background threads" \
    "$(printf 'help &\nhelp &\njobs &\nexit\n')")
check_exit "exit with background threads does not hang" "0" "$?"

# Spawn a background process AND a background thread, then exit.
out=$(timed_run 5 "exit with mixed background jobs" \
    "$(printf '/usr/bin/sleep 60 &\nhelp &\nexit\n')")
check_exit "exit with mixed jobs does not hang" "0" "$?"

# Kill any stray sleep processes we may have left
pkill -f "sleep 60" 2>/dev/null || true

# ── 9. Mutex / no-race spot-check: rapid sequential cd ────────────────────────
echo ""
echo "9. Rapid sequential cd (cwd consistency)"

out=$(printf 'cd /tmp\ncd /var\ncd /usr\npwd\n' | "$SHELL_BIN" 2>/dev/null)
check "last cd wins — pwd shows /usr"  "/usr"  "$out"

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo "=================================================================="
total=$((PASS + FAIL))
printf " Results: %d / %d passed\n" "$PASS" "$total"
echo "=================================================================="

[[ $FAIL -eq 0 ]]
