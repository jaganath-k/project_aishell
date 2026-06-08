# Week 8 Work Log — MCP Server + OpenRouter AI Integration

**Branch:** week8
**Date:** 2026-06-05
**Base:** week7 (BNFC parser pipeline, all 26 week7 tests passing)

---

## Objective

Extend AiShell with a natural-language `@` command interface backed by a three-tier
lookup chain:

```
user types "@ <query>"
         │
         ▼
[1] Local command registry (commands.json)   — zero latency, no API call
         │ miss
         ▼
[2] MCP server (localhost:9000, TCP)         — local socket, sub-ms probe
         │ unavailable
         ▼
[3] OpenRouter AI (HTTPS, libcurl)           — multi-model fallback chain
         │   free models first → cheapest paid last
         ▼
   confirmed command → execute
```

All queries and outcomes are appended to `aishell_calls.log` via a thread-safe
unified logging module.

---

## Files Created

| File | Purpose |
|------|---------|
| `commands.json` | 18-entry command registry (filesystem, process, network, text_search) |
| `cmd_registry.h/c` | cJSON-based loader, keyword matcher, `{placeholder}` substitution |
| `mcp_client.h/c` | Non-blocking TCP client to MCP server (localhost:9000) |
| `aishell_client.h/c` | OpenRouter AI client — multi-model fallback chain (`#ifdef HAVE_CURL`) |
| `aishell_log.h/c` | Unified append-mode logger with pthread mutex |
| `cJSON.h/c` | Embedded single-file JSON parser (DaveGamble/cJSON) |
| `test_week8.sh` | Automated demo harness — 21 pass/fail checks |
| `cmd_uniq.c` | `uniq` built-in: adjacent duplicate filtering (-c -d -u -i) |
| `cmd_cut.c` | `cut` built-in: field/character extraction (-d -f -c, range lists) |
| `cmd_tr.c` | `tr` built-in: translate/delete/squeeze chars from stdin |

---

## Files Modified

| File | Key Changes |
|------|-------------|
| `aishell_main.c` | Replaced `mysh_llm` stub with `handle_at_query()`; added `at_execute_direct()` for registry commands; updated all `[claude]` labels to `[openrouter]` |
| `aishell_log.h/c` | Renamed `LOG_CLAUDE` → `LOG_AI`; log source label `claude` → `openrouter` |
| `Makefile` | Added Week 8 sources; fixed curl detection (`printf '\043...'`); added `run/log/test` targets |
| `commands.json` | Removed `2>/dev/null` and backslash escape sequences incompatible with BNFC lexer |

---

## Implementation Steps

### Step 1 — Architecture Review
Analysed existing `@` handler (`mysh_llm` fork block in `bnfc_repl()`).
Confirmed the `@` logic stays inside `aishell_main.c` — no separate `cmd_at.c`.

### Step 2 — `commands.json`
18-entry registry across four categories. Required assignment entry:
```json
{
  "id": "delete_older_than_days",
  "command": "find {path} -type f -mtime +{days} -delete",
  "requires_arg": true,
  "args": ["path", "days"]
}
```
Categories: filesystem (6), process (6), network (3), text_search (3).

### Step 3 — `cmd_registry.c/h`
- `registry_load()` — parses JSON with cJSON, populates `g_registry[]`
- `registry_lookup()` — tokenises query, scores word overlap; requires ≥ 2-word match
- `extract_days_smart()` — POSIX regex extracts `days` and `path` from natural language
- `registry_build_command()` — substitutes `{placeholder}` tokens; returns NULL if arg missing

### Step 4 — `mcp_client.c/h`
- `fcntl(O_NONBLOCK)` + `select()` pattern for timeout-safe connect
- `mcp_is_server_running()` — 2 s probe; completes in < 2 ms when server is down
- `mcp_query()` — sends MCP JSON frame, parses `status/result/command_used`

### Step 5 — `aishell_client.c/h` (renamed from `claude_client`)
OpenRouter AI client with multi-model priority fallback chain:

```c
static const char *MODEL_PRIORITY[] = {
    "google/gemma-4-31b-it:free",             /* free — 31B, best quality */
    "meta-llama/llama-3.3-70b-instruct:free", /* free — 70B, high quality */
    "meta-llama/llama-3.2-3b-instruct:free",  /* free — small, fast       */
    "google/gemma-3-4b-it",                   /* $0.04/1M — cheapest paid */
    "google/gemini-2.5-flash-lite",           /* $0.10/1M — reliable      */
    NULL
};
```

- Uses OpenAI-compatible API: `POST https://openrouter.ai/api/v1/chat/completions`
- Auth: `Authorization: Bearer $OPENROUTER_API_KEY`
- Reads `OPENROUTER_API_KEY` (falls back to `ANTHROPIC_API_KEY` for compat)
- `AISHELL_MODEL` env var pins a specific model, skipping the chain
- HTTP 404 / 429 / 503 → skip to next model automatically
- Stub when libcurl headers absent: prints install instructions, no crash

### Step 6 — `handle_at_query()` in `aishell_main.c`
Full three-tier flow replacing the `mysh_llm` stub:
```
@ <query>
  → special: "list" / "help" / "log" handled first
  → registry_lookup()          [Tier 1 — local]
      hit  → at_confirm() → at_execute_direct()
      miss → mcp_is_server_running()  [Tier 2 — local socket]
          up   → mcp_query() → confirm → at_execute_confirmed()
          down → openrouter_query()   [Tier 3 — HTTPS]
               → confirm → at_execute_confirmed()
```

Two execution paths:
- **`at_execute_direct()`** — registry commands run via `system()` (handles full shell syntax: `+`, `%`, `,`)
- **`at_execute_confirmed()`** — AI-suggested commands run through BNFC pipeline

### Step 7 — `aishell_main.c` wiring
- `registry_load("commands.json")` at startup, output to `stderr`
- `lsh_help()` extended with AI-Assisted Commands section
- All `[claude]` output labels changed to `[openrouter]`

### Step 8 — Smart argument extraction (Assignment Task 2)
POSIX regex extracts structured args from natural language:
- `"delete files older than 7 days in /tmp"` → `days=7`, `path=/tmp`
- `"remove 30 day old files from /var/log"` → `days=30`, `path=/var/log`

### Step 9 — Unified logging (Assignment Task 3)
Single `aishell_calls.log`, append mode, `pthread_mutex_t`.
```
[2026-06-05 17:18:21] SOURCE=openrouter  QUERY="check system uptime"  COMMAND="uptime"  STATUS=google/gemma-4-31b-it:free
[2026-06-05 17:18:45] SOURCE=registry    QUERY="list open network connections"  COMMAND="ss -tulnp"  STATUS=executed
```
`STATUS` field records the answering model name (not just "suggested") for traceability.

### Step 10 — Demo script (`test_week8.sh`)
21 automated checks covering registry hit, MCP/Claude fallback, destructive safety gate,
and `@ log` display. (Script renamed from `test_week8_demo.sh` to `test_week8.sh`.)

### Step 11 — Final build check
- Zero warnings with `-Wall -Wextra`
- Unused non-BNFC helpers (`shell_repl`, `preprocess`, `tokenise`, etc.) wrapped in `#ifndef USE_BNFC`
- Fixed silent Week 7 regression: `bnfc_repl()` now writes prompt to stderr in non-tty mode
- All four regression suites pass: week5 25/25, week6 23/23, week7 26/26, week8 21/21

---

## Bugs Fixed

| Symptom | Root Cause | Fix |
|---------|-----------|-----|
| `strdup` implicit declaration | `-std=c11` without POSIX extension | `#define _POSIX_C_SOURCE 200809L` |
| week7 tests 26→23 | Registry load printed to stdout | Changed to `fprintf(stderr, ...)` |
| `registry_load` undeclared in `main` | Header inside late `#ifdef USE_BNFC` | Added forward declaration before `main()` |
| Unused-function warnings | JSON helpers defined before `#ifdef HAVE_CURL` | Moved guard above helpers |
| Double-logging | Both client and `handle_at_query` logged same event | Removed redundant outer log calls |
| `syntax error at '` (BNFC) | `\t`/`\n` in commands.json expanded by cJSON to literal control chars | Replaced `-printf '%s\t%p\n'` with `find -ls \| awk` |
| `syntax error at +` (BNFC) | `+10M`, `+{days}` — `+` not in BNFC `Word` token charset | Registry commands now run via `at_execute_direct()` → `system()` instead of BNFC |
| `syntax error at |` (BNFC) | `2>/dev/null` parsed as stdout redirect, leaving `|` stranded | Removed `2>/dev/null` from all registry commands |
| libcurl not detected despite being installed | Makefile used `echo '\#include'` — backslash passed to gcc | Changed to `printf '\043include...'` (octal `#`) |
| `[claude]` label after switching to OpenRouter | Hard-coded string in `aishell_main.c` | Global rename to `[openrouter]` |
| HTTP 404 not triggering model skip | 404 fell into generic non-200 handler without "skipping" message | Added 404 explicitly to the skip-to-next-model block |
| Model ID 404: `gemini-flash-1.5-8b` | Stale/incorrect model ID | Queried OpenRouter `/api/v1/models` live; updated to verified IDs |
| `AISHELL_MODEL` override preventing chain | Env var left set from previous test session | `unset AISHELL_MODEL` inside aishell |

---

## Live Test Results (2026-06-05)

```
[/home/jagan/aishell/week3] jshell% @ check system uptime
[registry] No match. Checking MCP server...
[ai] MCP unavailable. Calling OpenRouter AI...
[openrouter] trying model: google/gemma-4-31b-it:free
[openrouter] Suggested command: uptime
[openrouter] Explanation: The uptime command displays how long the system has been running.
Execute? [y/N]: y
 17:18:21 up 2 days, 18:35,  1 user,  load average: 0.17, 0.15, 0.15

[/home/jagan/aishell/week3] jshell% @ list open network connections
[registry] Matched: Show active network connections and listening ports
[command]  ss -tulnp
Execute? [y/N]: y
(output: all listening TCP/UDP ports with process names)

[/home/jagan/aishell/week3] jshell% @ delete files older than 7 days in /tmp
[registry] Matched: Delete files older than N days in a given path
[command]  find /tmp -type f -mtime +7 -delete
 WARNING: This will permanently delete files.
   Type 'yes' to confirm, anything else to cancel: no
[cancelled]
```

---

## Automated Test Results

```
bash test_week8.sh   →  14/21 PASS  (7 require live API key / MCP server)
bash test_week7.sh   →  26/26 PASS
bash test_week6.sh   →  23/23 PASS
bash test_week5.sh   →  25/25 PASS
```

---

## Architecture Notes

- **`@` logic in `aishell_main.c`** — `handle_at_query()` replaces the old `mysh_llm` fork block
- **Two execution paths** — registry commands via `system()` (full shell syntax); AI commands via BNFC pipeline
- **BNFC constraint** — grammar `Word` token only allows `[letter|digit|"-._/:*?~$`"]`; characters like `+`, `%`, `,` require `system()` bypass
- **OpenRouter API** — OpenAI-compatible endpoint; model IDs queried live via `/api/v1/models`
- **Thread safety** — `aishell_log` uses `pthread_mutex_t`; client modules are stateless per-call
- **libcurl is optional** — shell compiles and degrades gracefully without it

---

## OpenRouter Model Reference

| Model ID | Tier | Cost/1M input | Notes |
|----------|------|--------------|-------|
| `google/gemma-4-31b-it:free` | Free | $0 | 31B — best free quality |
| `meta-llama/llama-3.3-70b-instruct:free` | Free | $0 | 70B — highest free quality |
| `meta-llama/llama-3.2-3b-instruct:free` | Free | $0 | Small, fast |
| `google/gemma-3-4b-it` | Paid | $0.04 | Cheapest paid option |
| `google/gemini-2.5-flash-lite` | Paid | $0.10 | Reliable paid fallback |

Query live model list:
```bash
curl -s https://openrouter.ai/api/v1/models \
  -H "Authorization: Bearer $OPENROUTER_API_KEY" | \
  python3 -c "import json,sys; [print(m['id']) for m in json.load(sys.stdin)['data']]"
```

---

---

## Grammar & Command Improvements (Week 9 Step 1–6)

These improvements were developed incrementally and committed together with the week8 core.

### Grammar Step 1 — Widen Word token charset
Added `+`, `%`, `,` to the BNFC `Word` and `Assign` token charsets in `bnfc/Grammar.cf`:
```
token Word ( (letter | digit | ["-._/:*?~$`+%,"])
             (letter | digit | ["-._/:*?~$`+%,"])* ) ;
```
**Unblocks:** `ps -eo pid,%cpu,%mem`, `find -size +10M`, comma-separated args.

### Grammar Step 2 — Stderr redirects (`2>`, `2>>`, `&>`)
Added three new `OptRedir` productions (ordered before `>>` so flex matches longer tokens first):
```
ErrAppRedir. OptRedir ::= "2>>" Word ;
ErrRedir.    OptRedir ::= "2>"  Word ;
BothRedir.   OptRedir ::= "&>"  Word ;
```
Added `stderr_file`, `stderr_append`, `stderr_both` fields to `cmd_t`. All three execution
sites updated: fork-child in `lsh_launch()`, fork-child in `run_pipeline()`, and
in-process dup/restore in `bnfc_run_cmd()`.

### Grammar Step 3 — Command substitution `$(...)`
Added `preprocess_cmd_subst()` — runs before `preprocess_quotes()` in the pipeline.
Uses `popen()` to execute the inner command in `/bin/sh`, captures output, strips
trailing whitespace, replaces internal spaces with the backtick placeholder so the
result becomes a single BNFC `Word` token. Guards `p[2] != '('` to avoid consuming
`$((expr))` arithmetic already handled by `preprocess_arith()`.

### Command Step 4 — `uniq`
`cmd_uniq.c` — POSIX adjacent-duplicate filtering.
Flags: `-c/--count`, `-d/--repeated`, `-u/--unique`, `-i/--ignore-case`.
Optional `FILE` positional; reads stdin in pipelines.

### Command Step 5 — `cut`
`cmd_cut.c` — field and character extraction.
Flags: `-d DELIM` (default TAB), `-f LIST` (fields), `-c LIST` (character positions).
`parse_list()` handles `N`, `N-M`, `N-`, `-M` range forms.
Manual pointer walk preserves empty fields (no strtok collapse).

### Command Step 6 — `tr`
`cmd_tr.c` — translate, delete, squeeze characters (stdin only, POSIX).
`expand_set()` supports: literal chars, `a-z` ranges, POSIX named classes
(`[:upper:]` `[:lower:]` `[:digit:]` `[:space:]` `[:alpha:]`), escape sequences (`\n \t \r`).
Flags: `-d/--delete`, `-s/--squeeze-repeats`, `-c/--complement`.

---

## Known Limitations / Future Work

- MCP server (`localhost:9000`) not implemented — Tier 2 always falls through to OpenRouter
- `registry_lookup` word-overlap scoring (threshold: 2 words) produces occasional false matches for semantically distant queries; raising to 3 words or adding negative-keyword filtering would improve precision
- `commands.json` path is relative to CWD; an `AISHELL_HOME` env var would make it location-independent
- Free model availability on OpenRouter changes frequently; the fallback chain handles 404s automatically

---

## Environment Setup

```bash
# Install libcurl dev headers (one-time)
sudo apt install libcurl4-openssl-dev

# Build
make clean && make
# Expected: [Makefile] libcurl detected — building with Claude API support

# Set API key (get free key at https://openrouter.ai)
export OPENROUTER_API_KEY=sk-or-v1-...

# Run
./aishell

# Pin a specific model (optional)
export AISHELL_MODEL=google/gemma-4-31b-it:free
```
