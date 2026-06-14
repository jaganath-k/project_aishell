# AiShell — Week 9 Work Log
## MCP Server + FTP Protocol + RAG-Grounded `@` Command

---

## Overview

Week 9 extended AiShell with three major new capabilities built on top of the Week 8 MCP client, OpenRouter AI integration, and BNFC grammar shell:

| Capability | What it does |
|------------|-------------|
| **MCP/FTP Server** | TCP server on port 9000. Accepts FTP clients (USER/QUIT/PORT/STOR/RETR/LIST/MKD) and MCP JSON tool clients from the same port. One detached pthread per connection. |
| **RAG-Grounded `@`** | Replaced keyword matching with TF-IDF cosine similarity retrieval. Builds a vector index from `commands.json` at startup. High-confidence matches execute directly; low-confidence matches inject retrieved context into the LLM prompt. |
| **Config System** | `aishell.conf` key=value file with live runtime updates via `server config key=value`. New `server` built-in command. |

---

## Files Created This Week

| File | Role |
|------|------|
| `mcp_server.h` | Server types (`ClientContext`) and public API (`mcp_server_start/stop/is_running`) |
| `mcp_server.c` | TCP listener, client dispatch (FTP vs MCP JSON), 4 MCP tool handlers, security checks |
| `ftp_handler.h` | Single declaration: `handle_ftp_session(int ctrl_fd, const char *first_line)` |
| `ftp_handler.c` | Full FTP protocol: USER/QUIT/PORT/STOR/RETR/LIST/MKD + all safety checks |
| `rag_retriever.h` | RAG API: `rag_build_index()`, `rag_query()`, `rag_build_context()` |
| `rag_retriever.c` | TF-IDF index build + cosine similarity query — pure C, no external ML libs |
| `config.h` | `AiShellConfig` struct + `config_load/save/print/set` API |
| `config.c` | Config loader/saver with defaults, validation, and live update support |
| `cmd_server.c` | `server status/start/stop/config` built-in command |
| `aishell.conf` | Default configuration file (auto-created if missing) |
| `test_week9.sh` | Full test suite: Part A — 15 server/FTP/MCP/config acceptance tests; Part B — 40 shell regression checks (xargs, read, test/[, alias, history, arithmetic, process substitution) |
| `WORKLOG_WEEK9.md` | This file |

---

## Files Modified This Week

| File | What Changed |
|------|-------------|
| `aishell_main.c` | Added `--server` daemon flag; `config_load()` + `rag_build_index()` + `mcp_server_start()` in `main()`; RAG-powered STEP B in `handle_at_query()`; `AssignArg` handler in `expand_arg_ps()`; single-quote support in `preprocess_quotes()`; architecture comment updated to Week 9 |
| `aishell_client.c` | `try_model()` now accepts optional `context` parameter; added `openrouter_query_ctx()`; RAG context prepended to system prompt when provided; `body` buffer increased to 8192 bytes |
| `aishell_client.h` | Added `openrouter_query_ctx(const char *query, const char *context)` declaration |
| `cmd_registry.c` | Added `registry_for_each()` and `registry_count()` for RAG index iteration |
| `cmd_registry.h` | Added declarations for the two new iteration functions |
| `bnfc/Grammar.cf` | Added `AssignArg. Arg ::= Assign ;` (allows `key=value` as command args); added `\` to `Word` charset (allows `\r\n` escape sequences in arguments) |
| `ftp_handler.c` | Fixed: path and existence validation now runs before PORT check in `cmd_retr()` — security checks always reply 550 regardless of PORT state |
| `Makefile` | Added `config.c mcp_server.c ftp_handler.c rag_retriever.c cmd_server.c` to SRCS; added `-lm` to LDFLAGS; added `test9`, `testall`, `ftp-test` targets |

---

## Architecture Implemented

### MCP Server — Client Detection

The server uses a 300ms `MSG_PEEK` timeout to distinguish client types without breaking either protocol:

```
Client connects to port 9000
  │
  ├─ recv(MSG_PEEK, 300ms)
  │    ├─ first byte == '{': MCP JSON client
  │    │    └─ read full line → dispatch to tool handler
  │    └─ timeout / other first byte: FTP client (server-speaks-first)
  │         └─ send "220 ready" banner → read FTP commands
```

**Why peek instead of reading:** FTP is server-speaks-first (client waits for the 220 greeting before sending anything). If we read first, FTP clients would hang. The peek timeout detects which protocol is in use.

### FTP Handler

```
handle_ftp_session(ctrl_fd, first_line)
  │
  ├─ send "220 AiShell FTP Service (Week 9) ready.\r\n"
  ├─ USER  → set username, reply "230 logged in"
  ├─ PORT  → parse 6 octets, always override IP to 127.0.0.1, store data_port
  ├─ LIST  → open data connection, run ls -la, send output, close
  ├─ MKD   → mkdir(2), reply "257" or "550 already exists"
  ├─ STOR  → validate path, open file, open data connection, read→write loop
  ├─ RETR  → validate path, open file (→550 if missing), open data connection, read→write loop
  ├─ PWD   → reply "257 \"<cwd>\""
  ├─ QUIT  → reply "221 Goodbye", return
  └─ other → "500 Unknown command"
```

### RAG @ Pipeline

```
@ find large files
  │
  ├─ rag_build_index() [at startup, not per query]
  │    ├─ tokenize each command's description + aliases + command string
  │    ├─ remove stop words (the, a, an, is, to, for, in, of, and, or, not...)
  │    ├─ compute TF per document
  │    ├─ compute IDF: log((1+N)/(1+df[w]))+1
  │    ├─ TF-IDF vectors: tfidf[d][w] = tf * idf[w]
  │    └─ L2-normalize each document vector
  │
  ├─ rag_query(query, results, top_k) [per @ invocation]
  │    ├─ tokenize query (read-only vocab lookup — no new words added)
  │    ├─ build query TF-IDF vector, apply IDF, L2-normalize
  │    ├─ cosine_sim[d] = dot(query_vec, doc_vec[d])  [both L2-normalized]
  │    └─ partial selection sort → return top-K above RAG_MIN_SCORE
  │
  └─ score-based dispatch
       ├─ score ≥ 0.5: high confidence → show match, confirm, execute
       ├─ score 0.1–0.5: low confidence → rag_build_context() → LLM with context
       └─ score < 0.1: no match → LLM without context
```

### Config System

```
config_load("aishell.conf")
  ├─ file exists: parse key=value lines, skip # comments
  └─ file missing: apply defaults, call config_save() to create it

config_set("aishell.conf", "server_timeout=60")
  ├─ validate key against known list
  ├─ validate value range (port: 1024-65535, max_clients: 1-64, timeout: 5-300)
  ├─ update g_config in memory
  └─ call config_save() to persist

server config [key=value]  ← calls config_print() or config_set()
```

---

## Security Hardening

All hardening was implemented during the initial coding of Steps 2–3, then verified in Step 9.

| Check | Location | Implementation |
|-------|----------|---------------|
| Localhost-only MCP | `mcp_server.c:peer_is_localhost()` | `getpeername()` checks peer addr == `127.0.0.1` |
| Command allowlist | `mcp_server.c:is_allowed_command()` | Static `ALLOWLIST[]`; `run_command` returns error if not in list |
| Shell injection | `mcp_server.c:is_safe_args()` | Rejects `;`, `&&`, `\|\|`, `` ` ``, `$(`, `>&` in MCP args |
| FTP path traversal | `ftp_handler.c:is_safe_filename()` | Rejects `..` components and NUL bytes; max 255 chars |
| FTP path escape | `ftp_handler.c:path_escapes_cwd()` | `realpath()` + `strncmp()` against `session.cwd` |
| FTP PORT hijack | `ftp_handler.c:cmd_port()` | IP always overridden to `127.0.0.1`; ports ≤ 1023 rejected |
| Binary-safe transfer | `ftp_handler.c:cmd_stor/cmd_retr()` | `read()`/`write()` not `fread`/`fputs` — NUL bytes preserved |
| Idle timeout | `mcp_server.c:client_handler()` | `SO_RCVTIMEO` = `MCP_RECV_TIMEOUT` seconds on each client fd |
| Oversized FTP input | `ftp_handler.c:handle_ftp_session()` | Lines > 1024 bytes → `500 Syntax error` |
| Oversized MCP args | `mcp_server.c:tool_run_command()` | args > 512 bytes truncated before popen |

---

## Grammar Changes (Week 9)

### Problem 1: `key=value` as command argument

**Symptom:** `server config server_timeout=60` → `syntax error at server_timeout=60`

**Root cause:** The BNFC grammar tokenizes `NAME=VALUE` as an `Assign` token (shell variable assignment). The `Arg` rule only accepted `Word`, `ArithExp`, `ProcSubstIn`, `ProcSubstOut` — not `Assign`.

**Fix:**
- Added `AssignArg. Arg ::= Assign ;` to `bnfc/Grammar.cf`
- Added `case is_AssignArg:` in `expand_arg_ps()` — passes the token as a literal string argument (no variable assignment performed)
- Regenerated parser: `make bnfc-gen` + bison + flex

### Problem 2: Backslash in arguments

**Symptom:** `printf 'hello\r\nworld'` → syntax error because `\` not in `Word` charset

**Root cause:** The `Word` token pattern did not include `\` (backslash), so tokens containing escape sequences like `\r\n` failed to lex.

**Fix:** Added `\\` to the `Word` token character class in `bnfc/Grammar.cf`, then regenerated parser.

### Problem 3: Single-quoted strings

**Symptom:** `printf 'USER testuser\r\nQUIT\r\n'` → `syntax error at '`

**Root cause:** `preprocess_quotes()` only handled `"..."` double-quoted strings. The `'` character was not in the `Word` charset and was not preprocessed.

**Fix:** Extended `preprocess_quotes()` in `aishell_main.c` to handle both `"..."` and `'...'`. Both quote types: strip the quote characters, replace internal spaces with backtick placeholder.

> **Note:** Single-quoted content still goes through `expand_vars()` — `$VAR` inside `'...'` is still expanded. Not fully POSIX-correct but covers the common use case.

---

## Bugs Found and Fixed

| Bug | Root Cause | Fix |
|-----|-----------|-----|
| `RETR ../../../etc/passwd` returned `425` instead of `550` | PORT check ran before path validation in `cmd_retr()` | Moved path validation and `open()` before the PORT check |
| `RETR nonexistent.txt` returned `425` instead of `550` | Linux `realpath()` succeeds for non-existent files if parent directory exists — `path_escapes_cwd()` passed, reaching PORT check | Open the file fd before PORT check; `open()` failure → `550 File not found` immediately |
| `./aishell --server &` — server exits immediately | Background process gets SIGTTIN; `bnfc_repl()` returns EOF, calling `mcp_server_stop()` | Added `--server` flag: starts server, blocks on `sigwait(SIGTERM/SIGINT)`, no REPL |
| STOR binary test file was empty | Test used `nc -l PORT > file` (receive mode) but STOR requires client to SEND data | Corrected: `nc -l PORT < /tmp/nultest.bin` for STOR; `nc -l PORT > /tmp/retrieved.bin` for RETR |
| `server config server_timeout=60` syntax error | `NAME=VALUE` tokenized as `Assign`, not accepted as `Arg` | Added `AssignArg. Arg ::= Assign ;` to grammar + `case is_AssignArg:` in executor |
| `printf 'hello\r\nworld'` syntax error | `'` not preprocessed; `\` not in `Word` charset | Single-quote preprocessing + `\` added to `Word` charset |

---

## What Claude Code Generated

| Step | Output |
|------|--------|
| Step 1 | Architectural review (no code) — identified insertion points, confirmed no conflict with `mcp_client.c` |
| Step 2 | `mcp_server.c` + `mcp_server.h` |
| Step 3 | `ftp_handler.c` + `ftp_handler.h` |
| Step 4 | `config.c` + `config.h` + `cmd_server.c` + patches to `aishell_main.c` + Makefile updates |
| Step 5 | `rag_retriever.c` + `rag_retriever.h` + `registry_for_each()` / `registry_count()` in `cmd_registry.c` |
| Step 6 | Updated `handle_at_query()` with RAG STEP B + `openrouter_query_ctx()` in `aishell_client.c` |
| Step 7 | MCP tool JSON schemas in `tool_list_tools()` with full JSON Schema for all 4 tools |
| Step 8 | `aishell.conf` default config file |
| Step 9 | Hardening audit — confirmed all checks in place; fixed RETR order bug |
| Step 10 | `test_week9.sh` (Part A: 15 acceptance demos + Part B: 40 regression checks) + this work log |
| Step 11 | `--server` daemon flag; grammar fixes for `'`, `\`, `key=value`; final clean build |

---

## Demo Results

All 15 acceptance tests pass (`bash test_week9_demo.sh`):

```
=== DEMO 1  — Server starts and listens on port 9000      PASS
=== DEMO 2  — FTP: 220 greeting and USER login            PASS  PASS
=== DEMO 3  — FTP: MKD creates directory                  PASS  PASS
=== DEMO 4  — FTP: input validation (traversal, missing)  PASS  PASS
=== DEMO 5  — MCP JSON: list_tools                        PASS  PASS
=== DEMO 6  — MCP JSON: get_status                        PASS
=== DEMO 7  — MCP JSON: run_command allowlist             PASS  PASS
=== DEMO 8  — server command built-in                     PASS
=== DEMO 9  — config: server config command               PASS
=== DEMO 10 — Regression: test_week9.sh still passes      PASS

Passed: 15   Failed: 0
```

---

## Known Limitations

| Limitation | Detail |
|------------|--------|
| Active mode FTP only | PASV (passive mode) not implemented; client must send PORT before STOR/RETR/LIST |
| TF-IDF RAG | Bag-of-words similarity only — no semantic/neural embeddings; works well for command descriptions but misses synonyms |
| Allowlist not unified | `g_config.server_allowlist` is loaded from config but `mcp_server.c` uses its own hardcoded `ALLOWLIST[]` at runtime |
| Single-quote `$VAR` expansion | `$VAR` inside `'...'` is still expanded — not fully POSIX-correct |
| No TLS | Port 9000 is plaintext only — local/educational use only |
| MCP: one request per connection | Server handles one MCP JSON request per connection then closes |

---

## Next Week (Week 10)

- Unify `g_config.server_allowlist` with `mcp_server.c:ALLOWLIST[]`
- FTP passive mode (PASV) for data connections behind NAT/firewalls
- Persistent MCP sessions (multiple tool calls per connection)
- Agentic RAG loop (iterative retrieval + retry on low-confidence)
- Graph RAG vs vector search trade-offs
- Multi-step AI decisions via MCP tool chaining
- Fully POSIX-correct single-quote handling (suppress `$VAR` expansion inside `'...'`)
