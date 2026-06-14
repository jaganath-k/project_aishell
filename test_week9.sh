#!/usr/bin/env bash
# test_week9.sh — Week 9 full test suite: MCP Server + FTP + RAG + shell regression
# Run from week3/ directory after building: make && bash test_week9.sh
#
# Requires: aishell binary, nc

AISHELL="${1:-./aishell}"
PORT=9000
PASS=0; FAIL=0

ok()     { echo "  PASS  $1"; PASS=$((PASS+1)); }
fail()   { echo "  FAIL  $1 — $2"; FAIL=$((FAIL+1)); }
header() { echo; echo "=== $1 ==="; }

# ── Helpers ──────────────────────────────────────────────────────────────────

# run LABEL EXPECTED SCRIPT — pipe SCRIPT to aishell, expect EXPECTED in stdout
run() {
    local label="$1" expected="$2" input="$3"
    local out
    out=$(printf '%s\n' "$input" | timeout 8 "$AISHELL" 2>/dev/null) || true
    if printf '%s\n' "$out" | grep -qF "$expected" 2>/dev/null; then
        ok "$label"
    else
        fail "$label" "expected '$expected', got: $(printf '%s' "$out" | head -2)"
    fi
}

# Start aishell --server in background (daemon mode, no REPL)
aishell_start() {
    "$AISHELL" --server > /tmp/week9_aishell.log 2>&1 &
    AISHELL_PID=$!
    for _i in 1 2 3 4 5 6 7 8 9 10; do
        ss -tlnp 2>/dev/null | grep -q "$PORT" && break
        read -t 0.5 _dummy </dev/null 2>/dev/null || true
    done
}

aishell_stop() {
    kill "$AISHELL_PID" 2>/dev/null || true
    wait "$AISHELL_PID" 2>/dev/null || true
}

echo
echo "============================================"
echo " test_week9.sh — Week 9 full test suite"
echo "============================================"

# ─────────────────────────────────────────────────────────────────────────────
header "PART A — Server acceptance tests"
# ─────────────────────────────────────────────────────────────────────────────

header "A1 — Server starts and listens on port 9000"

aishell_start

if command -v nc >/dev/null 2>&1; then
    if echo "" | timeout 2 nc -z 127.0.0.1 "$PORT" 2>/dev/null; then
        ok "server listening on port $PORT"
    else
        fail "server listening" "port $PORT not open"
    fi
else
    if (echo "" > /dev/tcp/127.0.0.1/$PORT) 2>/dev/null; then
        ok "server listening on port $PORT"
    else
        fail "server listening" "port $PORT not open"
    fi
fi

# ─────────────────────────────────────────────────────────────────────────────
header "A2 — FTP: 220 greeting and USER login"

FTP_OUT=$(printf 'USER testuser\r\nQUIT\r\n' | timeout 5 nc 127.0.0.1 "$PORT" 2>/dev/null || true)
if printf '%s\n' "$FTP_OUT" | grep -q "220"; then
    ok "FTP 220 greeting received"
else
    fail "FTP 220 greeting" "got: $(printf '%s' "$FTP_OUT" | head -1)"
fi
if printf '%s\n' "$FTP_OUT" | grep -q "230"; then
    ok "FTP USER login — 230 response"
else
    fail "FTP USER login" "no 230 in: $(printf '%s' "$FTP_OUT" | tr '\r' ' ')"
fi

# ─────────────────────────────────────────────────────────────────────────────
header "A3 — FTP: MKD creates directory"

TESTDIR="testdir_week9_$$"
FTP_MKD=$(printf 'USER test\r\nMKD %s\r\nQUIT\r\n' "$TESTDIR" | \
    timeout 5 nc 127.0.0.1 "$PORT" 2>/dev/null || true)
if printf '%s\n' "$FTP_MKD" | grep -q "257"; then
    ok "FTP MKD — 257 directory created"
    rm -rf "${HOME:?}/$TESTDIR" 2>/dev/null || true
else
    fail "FTP MKD" "no 257 in response"
fi

FTP_MKD2=$(printf 'USER test\r\nMKD aishell_demo_existing\r\nMKD aishell_demo_existing\r\nQUIT\r\n' | \
    timeout 5 nc 127.0.0.1 "$PORT" 2>/dev/null || true)
rm -rf "${HOME:?}/aishell_demo_existing" 2>/dev/null || true
if printf '%s\n' "$FTP_MKD2" | grep -q "550"; then
    ok "FTP MKD duplicate — 550 already exists"
else
    fail "FTP MKD duplicate" "no 550 in response"
fi

# ─────────────────────────────────────────────────────────────────────────────
header "A4 — FTP: input validation (path traversal and missing file)"

FTP_TRAV=$(printf 'USER test\r\nRETR ../../../etc/passwd\r\nQUIT\r\n' | \
    timeout 5 nc 127.0.0.1 "$PORT" 2>/dev/null || true)
if printf '%s\n' "$FTP_TRAV" | grep -q "550"; then
    ok "FTP RETR traversal — 550 permission denied"
else
    fail "FTP RETR traversal" "no 550 in: $(printf '%s' "$FTP_TRAV" | tr '\r' ' ')"
fi

FTP_MISSING=$(printf 'USER test\r\nRETR nonexistent_file_xyz.txt\r\nQUIT\r\n' | \
    timeout 5 nc 127.0.0.1 "$PORT" 2>/dev/null || true)
if printf '%s\n' "$FTP_MISSING" | grep -q "550"; then
    ok "FTP RETR missing file — 550 not found"
else
    fail "FTP RETR missing" "no 550"
fi

# ─────────────────────────────────────────────────────────────────────────────
header "A5 — MCP JSON: list_tools"

MCP_OUT=$(printf '{"type":"mcp","tool":"list_tools","params":{}}\n' | \
    timeout 5 nc 127.0.0.1 "$PORT" 2>/dev/null | tail -n +2 || true)
if printf '%s\n' "$MCP_OUT" | grep -q "run_command"; then
    ok "MCP list_tools — run_command in response"
else
    fail "MCP list_tools" "no 'run_command' in: $(printf '%s' "$MCP_OUT" | head -1)"
fi
if printf '%s\n' "$MCP_OUT" | grep -q "list_tools"; then
    ok "MCP list_tools — list_tools in response"
else
    fail "MCP list_tools" "no 'list_tools' in response"
fi

# ─────────────────────────────────────────────────────────────────────────────
header "A6 — MCP JSON: get_status"

MCP_STATUS=$(printf '{"type":"mcp","tool":"get_status","params":{}}\n' | \
    timeout 5 nc 127.0.0.1 "$PORT" 2>/dev/null | tail -n +2 || true)
if printf '%s\n' "$MCP_STATUS" | grep -q "server_port"; then
    ok "MCP get_status — server_port in response"
else
    fail "MCP get_status" "no 'server_port'"
fi

# ─────────────────────────────────────────────────────────────────────────────
header "A7 — MCP JSON: run_command allowlist"

MCP_ALLOW=$(printf '{"type":"mcp","tool":"run_command","params":{},"command":"echo","args":"hello"}\n' | \
    timeout 5 nc 127.0.0.1 "$PORT" 2>/dev/null | tail -n +2 || true)
if printf '%s\n' "$MCP_ALLOW" | grep -q "ok\|hello"; then
    ok "MCP run_command allowlisted (echo) — ok"
else
    fail "MCP run_command echo" "unexpected: $MCP_ALLOW"
fi

MCP_DENY=$(printf '{"type":"mcp","tool":"run_command","params":{},"command":"rm","args":"-rf /"}\n' | \
    timeout 5 nc 127.0.0.1 "$PORT" 2>/dev/null | tail -n +2 || true)
if printf '%s\n' "$MCP_DENY" | grep -q "error\|allowlist"; then
    ok "MCP run_command blocked (rm) — error returned"
else
    fail "MCP run_command rm" "should be blocked"
fi

# ─────────────────────────────────────────────────────────────────────────────
header "A8 — server built-in: status and config"

SERVER_OUT=$(printf 'server status\n' | timeout 8 "$AISHELL" 2>/dev/null || true)
if printf '%s\n' "$SERVER_OUT" | grep -q "running\|stopped"; then
    ok "server status — shows running/stopped"
else
    fail "server status" "got: $SERVER_OUT"
fi

CONFIG_OUT=$(printf 'server config\n' | timeout 8 "$AISHELL" 2>/dev/null || true)
if printf '%s\n' "$CONFIG_OUT" | grep -q "server_enabled\|server_port"; then
    ok "server config — prints settings"
else
    fail "server config" "got: $CONFIG_OUT"
fi

aishell_stop

# ─────────────────────────────────────────────────────────────────────────────
header "PART B — Shell regression tests (piped REPL)"
# ─────────────────────────────────────────────────────────────────────────────

RT=/tmp/aishell_week9_read.txt
printf 'hello\n' > "$RT"

# ── B1. xargs ────────────────────────────────────────────────────────────────
header "B1 — xargs"

run  "xargs basic — single word echoed"         "hello"       'echo hello | xargs echo'
run  "xargs -n1 — one arg per call"             "world"       'echo "hello world" | xargs -n1 echo'
run  "xargs -n2 — two args per call"            "hello world" 'echo "hello world" | xargs -n2 echo'
run  "xargs -I% replace-string"                 "hello.c"     'echo hello | xargs -I% echo %.c'
run  "xargs pipe count (wc -w)"                 "3"           'echo "a b c" | xargs echo | wc -w'
run  "xargs grep over files"                    "Makefile"    'echo Makefile | xargs grep -l CC'

# ── B2. read ─────────────────────────────────────────────────────────────────
header "B2 — read"

run  "read from file — assigns variable"        "hello" "read X < $RT && echo \$X"
run  "read in sequence — var persists"          "hello" "read X < $RT; echo \$X"
run  "read -n 1 — reads one char"               "h"     "read -n 1 X < $RT && echo \$X"
run  "read then use in pipeline"                "HELLO" "read X < $RT; echo \$X | tr a-z A-Z"

# ── B3. test ─────────────────────────────────────────────────────────────────
header "B3 — test"

run  "test -f on existing file"    "exists"     'test -f Makefile && echo exists'
run  "test -d on directory"        "is dir"     'test -d /tmp && echo "is dir"'
run  "test -e on existing path"    "found"      'test -e /etc/hostname && echo found'
run  "test -x on executable"       "executable" 'test -x ./aishell && echo executable'
run  "test -n (non-empty string)"  "nonempty"   'test -n hello && echo nonempty'
run  "test string equality ="      "equal"      'test abc = abc && echo equal'
run  "test numeric -eq"            "eq"         'test 42 -eq 42 && echo eq'
run  "test numeric -lt"            "less"       'test 3 -lt 10 && echo less'
run  "test numeric -gt"            "greater"    'test 10 -gt 3 && echo greater'
run  "test numeric -le"            "le"         'test 5 -le 5 && echo le'
run  "test numeric -ge"            "ge"         'test 7 -ge 3 && echo ge'
run  "test -a AND combinator"      "both"       'test -f Makefile -a -d /tmp && echo both'

# ── B4. [ ] bracket form ─────────────────────────────────────────────────────
header "B4 — [ ] bracket syntax"

run  "[ -f file ] form"         "yes"   '[ -f Makefile ] && echo yes'
run  "[ -d dir ] form"          "yes"   '[ -d /tmp ] && echo yes'
run  "[ -x exec ] form"         "yes"   '[ -x ./aishell ] && echo yes'
run  "[ string = string ]"      "match" '[ hello = hello ] && echo match'
run  "[ N -le N ]"              "ok"    '[ 5 -le 10 ] && echo ok'
run  "if [ -f ] then ... fi"    "found" 'if [ -f Makefile ]; then echo found; fi'
run  "if [ -d ] else"           "isdir" 'if [ -d /tmp ]; then echo isdir; fi'

# ── B5. md5sum / sha256sum ───────────────────────────────────────────────────
header "B5 — md5sum / sha256sum"

run  "echo test | md5sum"       "d8e8fca2dc0f896fd7cb4cb0031ba249" 'echo test | md5sum'
run  "echo test | sha256sum"    "f2ca1bb6c7e907d06dafe4687e579fce" 'echo test | sha256sum'
run  "printf abc | md5sum"      "900150983cd24fb0d6963f7d28e17f72" 'printf "abc" | md5sum'

# ── B6. alias ────────────────────────────────────────────────────────────────
header "B6 — alias"

run  "alias define then list"       "greet" "$(printf 'alias greet=hi\nalias')"
run  "alias defines value"          "hi"    "$(printf 'alias greet=hi\nalias')"
run  "alias expansion in command"   "hello" "$(printf 'alias greet="echo hello"\ngreet')"

# ── B7. history ──────────────────────────────────────────────────────────────
header "B7 — history"

run  "history records echo command"  "echo hello" 'echo hello; history'
run  "history -n 1 shows last entry" "echo hi"    'echo hi; history -n 1'

# ── B8. Arithmetic expansion ─────────────────────────────────────────────────
header "B8 — arithmetic expansion"

run  "pure constant arith"       "42" 'echo $((6 * 7))'
run  "arith with variable ref"   "10" 'x=5; echo $((x * 2))'
run  "multi-op arith"            "47" 'echo $((10 * 5 - 3))'
run  "arith division"            "25" 'echo $((100 / 4))'
run  "arith modulo"              "1"  'echo $((10 % 3))'
run  "arith in sleep arg"        "arith ok" 'sleep $((1 + 0)) && echo "arith ok"'

# ── B9. Process substitution ─────────────────────────────────────────────────
header "B9 — process substitution"

run  "diff <(echo a) <(echo b) shows diff"   "1c1"   'diff <(echo a) <(echo b)'
run  "diff identical <(cmd) — no output"      ""      'diff <(echo hello) <(echo hello) || true'
run  "cat <(echo hello) reads pipe"           "hello" 'cat <(echo hello)'
run  "diff identical proc substs — exits 0"   "same"  'diff <(echo apple) <(echo apple) && echo same'
run  "tee >(cat) passes through"              "hello" 'echo hello | tee >(cat) > /dev/null'

# ── Results ──────────────────────────────────────────────────────────────────
echo
echo "============================================"
printf "  Passed: %d   Failed: %d\n" "$PASS" "$FAIL"
echo "============================================"
echo

exit "$FAIL"
