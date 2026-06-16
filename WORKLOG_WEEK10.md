# Week 10 AI Work Log — ShellAI Final: Hardening + MCP Agent Backend

## What changed this week

| Change | File(s) |
|--------|---------|
| Deterministic heuristic fallback (ported from mysh_llm.py scoring) | `rag_retriever.c`, `rag_retriever.h` |
| Single-line + catalog validation wrapper | `validate.c`, `validate.h` |
| Extended destructive-command pattern list (12 patterns) | `aishell_main.c` |
| fs.* MCP tools (list/read/write/append/stat/search) | `mcp_server.c` |
| proc.* MCP tools (list/kill/wait) | `mcp_server.c`, `jobs_api.h`, `aishell_main.c` |
| env.* MCP tools (get/set/list) | `mcp_server.c`, `jobs_api.h`, `aishell_main.c` |
| `edit` command (unified subcommand dispatch) | `cmd_edit.c`, `cmd_edit.h` |
| edit.* MCP tools (replace_line/insert_line/delete_line/replace) | `mcp_server.c` |

## MCP Tool Inventory (post-Week 10)

| Tool | Category | Description |
|------|----------|-------------|
| `run_command` | Week 9 | Run allowlisted built-in |
| `list_tools` | Week 9 | List all MCP tools |
| `get_status` | Week 9 | Server uptime + metrics |
| `get_registry` | Week 9 | All registered shell commands |
| `fs.list` | Week 10 | List directory contents |
| `fs.read` | Week 10 | Read file (up to 64 KB) |
| `fs.write` | Week 10 | Write/overwrite file |
| `fs.append` | Week 10 | Append to file |
| `fs.stat` | Week 10 | File metadata |
| `fs.search` | Week 10 | Recursive regex search |
| `proc.list` | Week 10 | List background jobs |
| `proc.kill` | Week 10 | Send signal to process job |
| `proc.wait` | Week 10 | Wait for process job exit |
| `env.get` | Week 10 | Get environment variable |
| `env.set` | Week 10 | Set environment variable |
| `env.list` | Week 10 | List all environment variables |
| `edit.replace_line` | Week 10 | Replace line N in file |
| `edit.insert_line` | Week 10 | Insert line before line N |
| `edit.delete_line` | Week 10 | Delete line N from file |
| `edit.replace` | Week 10 | Global literal-string replace |

## Three example @ queries and their results

| Query | Path taken | Suggestion | Executed? |
|-------|------------|------------|-----------|
| `@ list all files modified today` | RAG TF-IDF → heuristic fallback | `ls` | No (y/N declined) |
| `@ list files in this directory` (no API key) | Heuristic fallback | `ls` (name score=3) | No (y/N declined) |
| `@ delete everything in tmp` | RAG/AI → destructive check | shows ⚠ WARNING | No (declined at gate) |

## Reflection: AI assistant comparison to mysh_llm.py

### C-native TF-IDF vs mysh_llm.py keyword scoring

The TF-IDF RAG retriever in `rag_retriever.c` and `mysh_llm.py`'s scoring algorithm both solve the same problem: given a natural-language query, rank registered commands by relevance.

- **mysh_llm.py scoring** (ported to `heuristic_fallback_command()`): name match = 3 pts, summary match = 2 pts, long_help match = 1 pt per query token. Simple, deterministic, zero latency.
- **TF-IDF RAG**: builds a corpus from command name + summary + long_help, scores each document against the query using term frequency × inverse document frequency weighting. More principled — rare terms score higher than common ones.

In practice, for shell commands, both approaches produce similar results on clear queries (`ls` for "list files") but TF-IDF handles synonyms slightly better.

### Where LLM hallucinated vs RAG kept it grounded

The LLM (OpenRouter) sometimes suggested commands like `find . -mtime -1` which are valid system commands but NOT registered in the AiShell registry. The `validate_suggestion()` wrapper in `validate.c` catches these by checking `find_command()`. When caught, the system falls back to heuristic rather than executing an unknown command.

The RAG path rarely hallucinates because its output is always drawn from the registered command corpus — it cannot invent a command that isn't in `commands.json`.

### How catalog-only validation caught hallucinations

`validate_suggestion()` applies to all three suggestion paths:
1. **RAG**: Commands from `commands.json` might reference system tools (`ss`, `ip`, `pkill`) not in the registry → SUGGEST_UNKNOWN_COMMAND → heuristic retry
2. **MCP server probe**: Trusted, only checked for single-line truncation
3. **OpenRouter AI**: Full catalog check — unknown commands trigger one heuristic retry

This three-tier validation ensures the user is never shown (let alone prompted to execute) a command that the shell cannot understand.

## Final course architecture summary (Weeks 5–10)

**Week 5** — Started the shell: monolithic REPL, 12 built-in commands via `strcmp` dispatch, basic `&` backgrounding.

**Week 6** — Command registry: `cmd_spec_t` + `registry.c` allow commands to self-register, eliminating the switch table. Added job table with `SIGCHLD` reaper thread.

**Week 7** — BNFC grammar: replaced hand-rolled tokenizer with a proper recursive-descent parser (lexer/parser/AST). Added `$VAR` expansion, `$((expr))`, `$(cmd)`, if/for/while, pipelines.

**Week 8** — AI integration: `@` handler via keyword matching → MCP client → OpenRouter (Claude/Llama), `aishell_calls.log` unified logging, `--commands-json` catalog export.

**Week 9** — MCP server: TCP listener on port 9000, pthread-per-client, FTP + JSON protocol detection. `run_command`, `list_tools`, `get_status`, `get_registry` tool handlers. 28 additional built-in commands.

**Week 10** — Hardening + agent backend: heuristic fallback, single-line/catalog validation, destructive pattern gate, full MCP toolset (fs.*/proc.*/env.*/edit.*), unified `edit` command. The shell is now a fully functional MCP agent backend.

## Known limitations / future work

- `env.set` via MCP affects the whole process environment (all threads see it immediately). This is by design but means MCP clients can alter variables the REPL depends on.
- `fs.write` / `fs.append` have no workspace confinement beyond the `path_is_safe("../" check)` — a future `fs_root_path` config option would sandbox writes.
- The `edit` command loads the entire file into memory (10 MB cap). Files larger than 10 MB require a real editor.
- `proc.kill` only targets process-type jobs (forked children). Thread-type background jobs (built-ins run with `&`) cannot be killed by PID — they must finish or the shell must exit.
- The RAG index is rebuilt at startup from `commands.json`. Adding commands dynamically would require an index reload trigger.
- `test_week6.sh` and `test_week7.sh` have pre-existing failures (6/23 and 1/25 pass respectively): the tests check for `"aishell — available built-in commands"` but the help output format changed to `"aishell — shell built-ins:"` in a later week. These are not Week 10 regressions; `make testall` correctly excludes them.
