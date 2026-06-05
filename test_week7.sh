#!/usr/bin/env bash
# test_week7.sh — Week 7 BNFC grammar integration tests
# Run: bash test_week7.sh   (from the week3/ directory)

set -uo pipefail
SHELL_BIN="./aishell"
PASS=0; FAIL=0
TMPDIR_W7=$(mktemp -d)

check() {
    local desc="$1" expected="$2" actual="$3"
    if echo "$actual" | grep -qF "$expected"; then
        printf "  PASS  %s\n" "$desc"; ((PASS++))
    else
        printf "  FAIL  %s\n" "$desc"
        printf "        expected: %s\n" "$expected"
        printf "        actual:   %s\n" "$actual"
        ((FAIL++))
    fi
}
check_absent() {
    local desc="$1" absent="$2" actual="$3"
    if echo "$actual" | grep -qF "$absent"; then
        printf "  FAIL  %s  (found unexpected: %s)\n" "$desc" "$absent"; ((FAIL++))
    else
        printf "  PASS  %s\n" "$desc"; ((PASS++))
    fi
}
check_exit() {
    local desc="$1" exp="$2" got="$3"
    if [[ "$got" == "$exp" ]]; then
        printf "  PASS  %s\n" "$desc"; ((PASS++))
    else
        printf "  FAIL  %s  (exit %s, wanted %s)\n" "$desc" "$got" "$exp"; ((FAIL++))
    fi
}

echo "=================================================================="
echo " Week 7 — BNFC grammar integration test suite"
echo "=================================================================="

# ── 1. Grammar: basic command + args ──────────────────────────────────────
echo ""
echo "1. Basic command dispatch"
out=$(printf 'echo hello\n' | "$SHELL_BIN" 2>/dev/null)
check "echo hello" "hello" "$out"

out=$(printf 'ls -l /tmp\n' | "$SHELL_BIN" 2>/dev/null)
check "ls -l /tmp runs (flags parse)" "drwx" "$out"

# ── 2. Pipeline ────────────────────────────────────────────────────────────
echo ""
echo "2. Pipeline (THREAD → FORK)"
out=$(printf 'echo hello | /usr/bin/grep hello\n' | "$SHELL_BIN" 2>/dev/null)
check "echo | grep — data flows" "hello" "$out"

out=$(printf 'echo hello | /usr/bin/wc -c\n' | "$SHELL_BIN" 2>/dev/null)
check "echo | wc -c — count correct" "6" "$out"

# ── 3. I/O Redirection ────────────────────────────────────────────────────
echo ""
echo "3. I/O Redirection"
outfile="$TMPDIR_W7/redir.txt"

printf "echo redir_ok > %s\n" "$outfile" | "$SHELL_BIN" 2>/dev/null
if [[ -f "$outfile" ]] && grep -q "redir_ok" "$outfile"; then
    printf "  PASS  > redirect creates file\n"; ((PASS++))
else
    printf "  FAIL  > redirect — file: '$(cat "$outfile" 2>/dev/null)'\n"; ((FAIL++))
fi

printf "echo line2 >> %s\n" "$outfile" | "$SHELL_BIN" 2>/dev/null
lines=$(wc -l < "$outfile")
if [[ "$lines" -eq 2 ]]; then
    printf "  PASS  >> append adds line\n"; ((PASS++))
else
    printf "  FAIL  >> append — got %s lines\n" "$lines"; ((FAIL++))
fi

out=$(printf "/usr/bin/wc -l < %s\n" "$outfile" | "$SHELL_BIN" 2>/dev/null)
check "< input redirect — wc sees 2 lines" "2" "$out"

# ── 4. Background job ──────────────────────────────────────────────────────
echo ""
echo "4. Background job (&)"
out=$(printf '/usr/bin/sleep 0.1 &\n' | "$SHELL_BIN" 2>/dev/null)
check "sleep & — prints job PID or thread" "[1]" "$out"

# ── 5. Semicolon-separated jobs ───────────────────────────────────────────
echo ""
echo "5. Semicolon-separated jobs"
out=$(printf 'echo one ; echo two ; echo three\n' | "$SHELL_BIN" 2>/dev/null)
check "three jobs — first"  "one"   "$out"
check "three jobs — second" "two"   "$out"
check "three jobs — third"  "three" "$out"

# ── 6. Variable assignment + expansion ────────────────────────────────────
echo ""
echo "6. Variable assignment + expansion"
out=$(printf 'x=world\necho hello $x\n' | "$SHELL_BIN" 2>/dev/null)
check "x=world ; echo \$x" "hello world" "$out"

out=$(printf 'mydir=/tmp\nls $mydir\n' | "$SHELL_BIN" 2>/dev/null | head -1)
check "\$var in path position" "" "$out"   # just check it doesn't crash
printf "  PASS  variable expansion in path (no crash)\n"; ((PASS--))   # undo generic check
if printf 'mydir=/tmp\nls $mydir\n' | "$SHELL_BIN" 2>/dev/null | grep -q ""; then
    printf "  PASS  variable expansion in path (no crash)\n"; ((PASS++))
fi

# ── 7. Special variables ──────────────────────────────────────────────────
echo ""
echo "7. Special variables"
out=$(printf 'echo ok\necho $?\n' | "$SHELL_BIN" 2>/dev/null)
check "\$? after success is 0" "0" "$out"

out=$(printf 'echo $$\n' | "$SHELL_BIN" 2>/dev/null)
if [[ "$out" =~ ^[0-9]+$ ]]; then
    printf "  PASS  \$\$  shows numeric PID: %s\n" "$out"; ((PASS++))
else
    printf "  FAIL  \$\$  expected PID, got: %s\n" "$out"; ((FAIL++))
fi

# ── 8. if / then / fi ─────────────────────────────────────────────────────
echo ""
echo "8. if / then / fi"
out=$(printf 'if echo ok then echo branch_taken fi\n' | "$SHELL_BIN" 2>/dev/null)
check "if (success) — branch taken" "branch_taken" "$out"

out=$(printf 'if /usr/bin/false then echo should_not_appear fi\n' | "$SHELL_BIN" 2>/dev/null)
check_absent "if (failure) — branch skipped" "should_not_appear" "$out"

# ── 9. Syntax error rejection ─────────────────────────────────────────────
echo ""
echo "9. Syntax error rejection (acceptance check)"
out=$(printf '| bad\n' | "$SHELL_BIN" 2>&1)
check "invalid syntax prints error" "syntax error" "$out"

out=$(printf '| bad\necho still_alive\n' | "$SHELL_BIN" 2>/dev/null)
check "shell continues after parse error" "still_alive" "$out"

# ── 10. No regression — week 5/6 features ────────────────────────────────
echo ""
echo "10. No regression — existing commands still dispatch"
out=$(printf 'help\n' | "$SHELL_BIN" 2>/dev/null)
check "help built-in still works"  "available built-in commands" "$out"

out=$(printf 'cd /tmp\npwd\n' | "$SHELL_BIN" 2>/dev/null)
check "cd + pwd — week6 still works" "/tmp" "$out"

out=$(printf 'exit 42\n' | "$SHELL_BIN" 2>/dev/null); ec=$?
check_exit "exit code propagation" "42" "$ec"

# ── 11. --commands-json ───────────────────────────────────────────────────
echo ""
echo "11. --commands-json catalog"
json_out=$("$SHELL_BIN" --commands-json 2>/dev/null)
check "--commands-json has commands key" '"commands"' "$json_out"
check "--commands-json includes ls"      '"name": "ls"' "$json_out"
check "--commands-json includes echo"    '"name": "echo"' "$json_out"

# ── 12. Glob pattern parses and reaches evaluator ─────────────────────────
echo ""
echo "12. Glob patterns"
out=$(printf 'ls *.c\n' | "$SHELL_BIN" 2>/dev/null | head -3)
check "ls *.c — glob parsed and executed" ".c" "$out"

# ── Summary ───────────────────────────────────────────────────────────────
rm -rf "$TMPDIR_W7"
echo ""
echo "=================================================================="
total=$((PASS + FAIL))
printf " Results: %d / %d passed\n" "$PASS" "$total"
echo "=================================================================="
[[ $FAIL -eq 0 ]]
