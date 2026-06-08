#!/usr/bin/env bash
# test_week8_demo.sh — Week 8 AiShell MCP + Claude AI Integration demo
#
# Demonstrates 3 natural-language @ queries end-to-end:
#   DEMO 1: Registry hit          — @ find large files
#   DEMO 2: MCP/Claude fallback   — @ check system uptime
#   DEMO 3: delete_older_than_days with destructive safety gate

set -u
SHELL_BIN="${1:-./aishell}"
LOG_FILE="aishell_calls.log"
PASS=0
FAIL=0

RED='\033[0;31m'
GRN='\033[0;32m'
YEL='\033[0;33m'
RST='\033[0m'

ok()   { printf "  ${GRN}PASS${RST}: %s\n" "$1"; ((PASS++)); }
fail() { printf "  ${RED}FAIL${RST}: %s\n" "$1"; ((FAIL++)); }
check() {
    local desc="$1" val="$2"
    if [ -n "$val" ]; then ok "$desc"; else fail "$desc"; fi
}

# ── helpers ───────────────────────────────────────────────────────────────────

log_entries_since() {
    # Print log lines that appeared after a given byte offset
    local offset="$1"
    tail -c +"$((offset + 1))" "$LOG_FILE" 2>/dev/null
}

log_offset() {
    # Current byte size of the log file (0 if absent)
    [ -f "$LOG_FILE" ] && wc -c < "$LOG_FILE" || echo 0
}

separator() {
    printf "\n${YEL}══════════════════════════════════════════════════════${RST}\n"
    printf "${YEL} %-52s${RST}\n" "$1"
    printf "${YEL}══════════════════════════════════════════════════════${RST}\n\n"
}

# ── verify shell binary ───────────────────────────────────────────────────────
if [ ! -x "$SHELL_BIN" ]; then
    echo "ERROR: $SHELL_BIN not found or not executable. Run 'make' first."
    exit 1
fi

# ── DEMO 1 ────────────────────────────────────────────────────────────────────
separator "DEMO 1: Registry hit — @ find large files"

echo "  Query: @ find large files"
echo "  Answering 'n' to keep demo non-destructive"
echo

PRE=$(log_offset)
OUTPUT=$(printf "@ find large files\nn\n" | "$SHELL_BIN" 2>/dev/null)
NEW_LOG=$(log_entries_since "$PRE")

echo "  Shell output:"
echo "$OUTPUT" | sed 's/^/    /'
echo

check "registry matched"              "$(echo "$OUTPUT" | grep -F '[registry] Matched')"
check "command shown"                 "$(echo "$OUTPUT" | grep -F '[command]')"
check "Execute? prompt shown"         "$(echo "$OUTPUT" | grep -F 'Execute?')"
check "log entry written"             "$(echo "$NEW_LOG" | grep -F 'SOURCE=registry')"
check "log STATUS not empty"          "$(echo "$NEW_LOG" | grep -F 'QUERY="find large files"')"
check "no MCP/Claude calls made"      "$([ -z "$(echo "$NEW_LOG" | grep -E 'SOURCE=mcp|SOURCE=claude')" ] && echo yes)"

# ── DEMO 2 ────────────────────────────────────────────────────────────────────
separator "DEMO 2: MCP/Claude fallback — @ check system uptime"

echo "  Query: @ check system uptime"
echo "  Expected: no registry match → MCP server probe → Claude API fallback"
echo "  (MCP server not running; Claude in stub mode without libcurl headers)"
echo

PRE=$(log_offset)
OUTPUT=$(printf "@ check system uptime\n" | "$SHELL_BIN" 2>/dev/null)
NEW_LOG=$(log_entries_since "$PRE")

echo "  Shell output:"
echo "$OUTPUT" | sed 's/^/    /'
echo

check "no registry match"             "$(echo "$OUTPUT" | grep -F '[registry] No match')"
check "MCP server probed"             "$(echo "$OUTPUT" | grep -E 'MCP unavailable|mcp')"
check "AI fallback attempted"         "$(echo "$OUTPUT" | grep -F '[ai] MCP unavailable')"
check "Claude path reached"           "$(echo "$OUTPUT" | grep -F '[claude]')"
check "log entry written"             "$(echo "$NEW_LOG" | grep -F 'SOURCE=claude')"
check "QUERY in log"                  "$(echo "$NEW_LOG" | grep -F 'check system uptime')"

# ── DEMO 3 ────────────────────────────────────────────────────────────────────
separator "DEMO 3: delete_older_than_days — safety gate + arg extraction"

echo "  Query: @ delete files older than 7 days in /tmp"
echo "  Answering 'no' to exercise the safety gate without deleting anything"
echo

PRE=$(log_offset)
OUTPUT=$(printf "@ delete files older than 7 days in /tmp\nno\n" | "$SHELL_BIN" 2>/dev/null)
NEW_LOG=$(log_entries_since "$PRE")

echo "  Shell output:"
echo "$OUTPUT" | sed 's/^/    /'
echo

check "registry matched delete entry" "$(echo "$OUTPUT" | grep -iF 'delete files older')"
check "path /tmp extracted"           "$(echo "$OUTPUT" | grep -F '/tmp')"
check "days 7 extracted"              "$(echo "$OUTPUT" | grep -F 'mtime +7')"
check "WARNING shown"                 "$(echo "$OUTPUT" | grep -iE 'warning|permanently')"
check "'yes' confirmation required"   "$(echo "$OUTPUT" | grep -iF 'yes')"
check "log entry written"             "$(echo "$NEW_LOG" | grep -F 'SOURCE=registry')"
check "log STATUS=cancelled"          "$(echo "$NEW_LOG" | grep -F 'STATUS=cancelled')"
check "command NOT executed (cancelled)" \
    "$([ -z "$(echo "$NEW_LOG" | grep -F 'STATUS=executed')" ] && echo yes)"

# ── @ log display check ───────────────────────────────────────────────────────
separator "BONUS: @ log display"

LOG_OUTPUT=$(echo "@ log" | "$SHELL_BIN" 2>/dev/null)
echo "  @ log output (last entries):"
echo "$LOG_OUTPUT" | sed 's/^/    /'
echo

check "@ log shows entries"           "$(echo "$LOG_OUTPUT" | grep -F 'SOURCE=')"

# ── Summary ───────────────────────────────────────────────────────────────────
separator "Results"
printf "  ${GRN}Passed: %d${RST}   ${RED}Failed: %d${RST}\n\n" "$PASS" "$FAIL"

if [ "$FAIL" -eq 0 ]; then
    printf "  ${GRN}All Week 8 demo checks passed.${RST}\n\n"
    exit 0
else
    printf "  ${RED}%d check(s) failed — see output above.${RST}\n\n" "$FAIL"
    exit 1
fi
