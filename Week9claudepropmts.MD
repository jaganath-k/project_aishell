# AiShell — Week 9: MCP Server + FTP-Style Protocol + RAG-Grounded @ Command
## Step-by-Step Prompts for Claude Code in VS Code

> **Prerequisites completed:**
> - Week 5: fork/exec, pipes, I/O redirection, signals, background jobs
> - Week 6: pthreads for built-ins, mutex-protected job table
> - Week 7: BNFC grammar parser, `@` stub with `mysh_llm`, `--commands-json`
> - Week 8: `@` full handler in `bnfc_repl()`, `commands.json` registry,
>   MCP client socket, OpenRouter/Claude API fallback, `aishell_calls.log`
> - Week 9 improvements: grammar fixes (Word token, 2>, &>), new commands
>   (uniq, cut, tr, grep, diff, tee, du, df, ln, chmod, sleep, which, etc.)

---

## What Week 9 Actually Requires (synthesised from all 4 sources)

| Source | What it contributes |
|---|---|
| **FTP_Service.pptx** | The core assignment: implement a socket **server** inside AiShell using FTP-style protocol (USER, QUIT, PORT, STOR, RETR, LIST, MKD). Server must use pthreads per connection. Test with the real Linux `ftp` client. |
| **FTP slides (assignment)** | Acceptance checks: server accepts + serves + closes cleanly; timeouts; config enable/disable; input validation; NUL-safe file transfer |
| **ModelContextProtocol.pptx** | The MCP framing: expose AiShell's built-in commands as MCP **tools** with JSON schema. The server IS the MCP server. Tools: `run_command`, `list_tools`, `get_status` |
| **RAG_EdgeAI.pptx** | Upgrade the `@` command: instead of a single keyword lookup, use **RAG** (retrieval-augmented generation) — embed the query, retrieve the most relevant command from a vector store built from `commands.json`, then pass context + query to the LLM |

---

## Week 9 Architecture

```
                    AiShell Process
                         │
          ┌──────────────┴──────────────┐
          │                             │
    REPL (bnfc_repl)             MCP Server Thread
    Week 7/8 unchanged           Listens on port 9000
          │                             │
     @ command                   accept() loop
     (Week 8 handler)            pthread per client
          │                             │
    RAG lookup (NEW)        FTP-style protocol handler
    embed query →                       │
    vector match →            ┌─────────┴─────────┐
    retrieve context →        │                   │
    LLM with context    FTP commands         MCP JSON tools
                        USER/QUIT/PORT       run_command
                        STOR/RETR/LIST       list_tools
                        MKD                  get_status
```

---

## STEP 1 — Architectural Review (Run First, No Code)

**Prompt to Claude Code:**
```
I am extending AiShell for Week 9. Here is my full current codebase:

[paste or @-reference: aishell_main.c, Makefile, cmd_registry.c,
 mcp_client.c, claude_client.c, aishell_log.c, commands.json]

What is already implemented (DO NOT recreate these):
- Week 5: fork/exec, pipes, I/O redirection, SIGCHLD/SIGINT, job table
- Week 6: pthreads for built-ins, pthread_mutex_t job_table_mutex
- Week 7: BNFC grammar (bnfc/Grammar.cf), preprocess_arith → preprocess_quotes
  → psInput pipeline, --commands-json registry output
- Week 8: @ handler in bnfc_repl() doing registry → MCP client → OpenRouter
  fallback, aishell_calls.log unified logger, commands.json with aliases
- Week 9 improvements already applied: Word token widened (+%,), 2>/&>
  redirects, uniq/cut/tr/grep/diff/tee/du/df/ln/chmod/sleep/which commands

Week 9 adds THREE new things:
1. MCP SERVER — a TCP server thread listening on port 9000 that accepts
   FTP-style protocol connections AND responds to MCP JSON tool calls.
   Uses pthreads (one thread per client connection).
2. FTP SERVICE — implements USER, QUIT, PORT, STOR, RETR, LIST, MKD
   commands over the control connection (port 9000 for control,
   dynamic port for data). Testable with the real Linux ftp client.
3. RAG-GROUNDED @ — upgrades the @ handler to use vector similarity
   matching against commands.json embeddings before calling the LLM,
   so the LLM gets relevant context (not just keyword matching).

Before writing any code:
1. Identify where in aishell_main.c the server thread should be started
   (hint: in main(), before bnfc_repl(), as a detached pthread)
2. Confirm we need these new files:
   mcp_server.c / mcp_server.h   — server listener + FTP protocol handler
   ftp_handler.c / ftp_handler.h — FTP command implementations
   rag_retriever.c / rag_retriever.h — vector embedding + cosine similarity
3. Confirm the Makefile changes needed
4. Flag any conflicts with existing Week 8 mcp_client.c
   (client and server are separate — confirm no port conflict)

Do not generate code yet. Confirm the plan.
```

---

## STEP 2 — Create the MCP Server Core (`mcp_server.c`)

> **Goal:** The TCP server that binds to port 9000, listens, and spawns a
> pthread for each client connection. Handles both FTP-style and MCP JSON clients.

**Prompt to Claude Code:**
```
Create mcp_server.c and mcp_server.h for AiShell Week 9.

This is the server-side complement to Week 8's mcp_client.c.
It listens on port 9000 and handles two types of clients:
  1. FTP clients (the real Linux ftp command — sends ASCII FTP commands)
  2. MCP JSON clients (Week 8's mcp_client.c — sends JSON tool calls)

Requirements for mcp_server.h:
  #define MCP_SERVER_PORT   9000
  #define MCP_BACKLOG       10
  #define MCP_RECV_TIMEOUT  30     // seconds before idle client is dropped
  #define MCP_MAX_CLIENTS   16

  typedef struct {
    int  client_fd;
    struct sockaddr_in client_addr;
    int  client_id;
  } ClientContext;

  int  mcp_server_start(void);   // start server in background pthread
  void mcp_server_stop(void);    // signal server to shut down
  int  mcp_server_is_running(void);

Requirements for mcp_server.c:

1. mcp_server_start():
   - Create TCP socket: socket(AF_INET, SOCK_STREAM, 0)
   - Set SO_REUSEADDR to allow restart without waiting
   - Bind to 0.0.0.0:MCP_SERVER_PORT
   - Listen with MCP_BACKLOG
   - Launch a server loop in a NEW detached pthread:
       pthread_t server_tid;
       pthread_create(&server_tid, NULL, server_loop, NULL);
       pthread_detach(server_tid);
   - Return 0 on success, -1 on error
   - Print: "[mcp-server] Listening on port 9000"

2. server_loop() (the pthread function):
   - accept() loop — for each connection:
     a. Allocate a ClientContext on the heap
     b. Launch a NEW pthread for the client:
          pthread_create(&tid, NULL, client_handler, ctx)
          pthread_detach(tid)   // self-cleaning thread
     c. Log: "[mcp-server] client N connected from IP:port"
   - Use a volatile sig_atomic_t server_running flag
     so mcp_server_stop() can cleanly break the loop

3. client_handler(void *arg):
   - Cast arg to ClientContext*
   - Set a recv timeout using setsockopt(SO_RCVTIMEO, MCP_RECV_TIMEOUT)
   - Read the first line from the client
   - Detect client type:
       If line starts with '{' → MCP JSON client → call handle_mcp_json(fd, line)
       Otherwise              → FTP client      → call handle_ftp_session(fd, line)
   - On return, close(client_fd) and free(ctx)
   - Log: "[mcp-server] client N disconnected"

4. handle_mcp_json(int fd, const char *first_line):
   - Parse the JSON tool call (manual string search — no lib needed):
       look for "tool" field → get tool name
       look for "params" → get query string
   - Dispatch to tool handlers:
       "run_command"  → run the command via popen(), capture output
       "list_tools"   → return JSON array of all registered command names
       "get_status"   → return JSON with uptime, command count, log tail
   - Build JSON response:
       {"status":"ok","result":"<output>","tool":"<name>"}
   - Send response with length-prefix framing (same as Week 8 protocol):
       snprintf(frame, sizeof(frame), "%zu\n%s\n", strlen(body), body)
       send() in a loop
   - For run_command: ONLY allow commands in the registry allowlist
     (never execute arbitrary commands — check against g_registry[])

5. Add a global allowlist check:
   static int is_allowed_command(const char *cmd) {
     // check if cmd starts with a name in the registry
     // return 1 if allowed, 0 if not
   }
   Any run_command request that fails the allowlist check returns:
   {"status":"error","message":"command not in allowlist"}

6. Logging: every tool call logged to aishell_calls.log via aishell_log()

Show mcp_server.c and mcp_server.h in full.
```

---

## STEP 3 — Implement the FTP Protocol Handler (`ftp_handler.c`)

> **Goal:** Handle real FTP protocol commands so the Linux `ftp` client can
> connect and interact with AiShell. This is the core FTP assignment from the slides.

**Prompt to Claude Code:**
```
Create ftp_handler.c and ftp_handler.h for AiShell Week 9.

This implements the FTP-style protocol over the control connection.
It must work with the real Linux ftp command:
  ftp 127.0.0.1 9000

FTP Response code format (NVT — Network Virtual Terminal):
  "200 Command OK.\r\n"
  "220 AiShell FTP Service ready.\r\n"
  "221 Goodbye.\r\n"
  "230 User logged in.\r\n"
  "250 Requested file action okay.\r\n"
  "257 Directory created.\r\n"
  "425 Can't open data connection.\r\n"
  "450 File unavailable.\r\n"
  "500 Syntax error.\r\n"
  "550 File not found.\r\n"

Implement handle_ftp_session(int ctrl_fd, const char *first_line):

This function runs the FTP command loop for one client connection.
State needed per session:
  typedef struct {
    int    ctrl_fd;          // control connection
    int    data_fd;          // data connection (-1 if not set)
    char   data_ip[64];      // from PORT command
    int    data_port;        // from PORT command
    char   username[64];     // from USER command
    char   cwd[512];         // current working directory (starts at HOME)
    int    logged_in;        // 1 after USER command
  } FtpSession;

Commands to implement:

USER <name>:
  Set session.username = name
  Reply: "230 User <name> logged in.\r\n"
  (No password check — teaching example only)
  Working directory stays as HOME regardless of username

QUIT:
  Reply: "221 Goodbye.\r\n"
  Close the control connection and return from handle_ftp_session()

PORT <a1,a2,a3,a4,p1,p2>:
  Parse the 6 comma-separated numbers
  IP = a1.a2.a3.a4  (but ALWAYS override with "127.0.0.1" for safety)
  Port = p1 * 256 + p2
  Store in session.data_ip and session.data_port
  Reply: "200 Port command successful.\r\n"

LIST [path]:
  Open a data connection to session.data_ip:session.data_port
  Run ls -l on session.cwd (or the given path relative to cwd)
  Send the ls output over the data connection
  Close the data connection
  Reply sequence:
    "150 Opening data connection.\r\n"  (before opening data conn)
    "226 Transfer complete.\r\n"        (after closing data conn)
  Use AiShell's built-in ls if available, else execvp("ls", args)

MKD <dirname>:
  Create directory at session.cwd/dirname using mkdir(2)
  If exists: Reply "550 Directory already exists.\r\n"
  If success: Reply "257 \"<path>\" directory created.\r\n"

STOR <filename>:
  Open a data connection to session.data_ip:session.data_port
  Reply: "125 Data connection already open.\r\n"
  Open file session.cwd/filename for writing (O_WRONLY|O_CREAT|O_TRUNC)
  Read from data connection in 4096-byte chunks, write to file
  Handle NUL bytes: use write() not fputs() — binary safe
  Close data connection and file
  Reply: "226 Transfer complete.\r\n"

RETR <filename>:
  Check file exists at session.cwd/filename
  If not: Reply "550 File not found.\r\n" and return
  Open data connection
  Reply: "150 Opening data connection.\r\n"
  Read file in 4096-byte chunks, send over data connection
  Handle NUL bytes — binary safe (read()/write() not fread/fprintf)
  Close data and file
  Reply: "226 Transfer complete.\r\n"

Data connection helper:
  static int open_data_connection(FtpSession *s):
    socket(AF_INET, SOCK_STREAM, 0)
    connect to s->data_ip : s->data_port with 10-second timeout
    return fd or -1 on failure

Input validation for ALL commands:
  - Reject filenames containing ".." (directory traversal)
  - Reject filenames containing NUL characters in the name itself
  - Reject paths that would escape session.cwd
  - Max filename length: 255 chars
  - If validation fails: Reply "550 Permission denied.\r\n"

Greeting on connect (send before reading any command):
  "220 AiShell FTP Service (Week 9) ready.\r\n"

Unknown command:
  "500 Unknown command.\r\n"

Show ftp_handler.c and ftp_handler.h in full.
```

---

## STEP 4 — Start the Server in `main()` and Add Config

> **Goal:** Start the MCP server thread at AiShell startup, add a config
> file (`aishell.conf`) and a `server` built-in command.

**Prompt to Claude Code:**
```
Integrate the MCP server into aishell_main.c and add configuration support.

Here is my current aishell_main.c: [paste full file]

Changes needed:

1. Add includes at the top:
   #include "mcp_server.h"
   #include "ftp_handler.h"
   #include "rag_retriever.h"   // (will be created in Step 5)

2. Create aishell.conf config file reader.
   Add a new file config.c / config.h with:

   typedef struct {
     int  server_enabled;       // default: 1
     int  server_port;          // default: 9000
     int  server_max_clients;   // default: 16
     int  server_timeout;       // default: 30
     char server_allowlist[512]; // comma-separated allowed commands
     int  rag_enabled;          // default: 1
     char log_file[256];        // default: "aishell_calls.log"
   } AiShellConfig;

   extern AiShellConfig g_config;

   void config_load(const char *path);   // reads aishell.conf
   void config_save(const char *path);   // writes aishell.conf
   void config_print(void);              // prints current config

   aishell.conf format (simple key=value):
     server_enabled=1
     server_port=9000
     server_max_clients=16
     server_timeout=30
     server_allowlist=ls,cat,grep,echo,find,du,df,ps
     rag_enabled=1
     log_file=aishell_calls.log

3. In main(), after registry_load(), add:
   // Load config
   config_load("aishell.conf");

   // Start MCP server if enabled
   if (g_config.server_enabled) {
     if (mcp_server_start() == 0)
       fprintf(stdout, "[aishell] MCP server started on port %d\n",
               g_config.server_port);
     else
       fprintf(stderr, "[aishell] Warning: could not start MCP server\n");
   }

4. In lsh_exit() or wherever exit is handled, add:
   mcp_server_stop();

5. Add a "server" built-in command (add to dispatch table):
   int lsh_server(char **args):
   Usage:
     server status    → print server running/stopped, port, connected clients
     server stop      → call mcp_server_stop()
     server start     → call mcp_server_start()
     server config    → call config_print()

6. Update help text:
   "  server <status|start|stop|config>   Manage the MCP/FTP server"

7. Update Makefile SRCS:
   Add: mcp_server.c ftp_handler.c config.c rag_retriever.c

Show all diffs to aishell_main.c, the new config.c/config.h,
the server built-in function, and the updated Makefile.
```

---

## STEP 5 — RAG Retriever for the `@` Command

> **Goal:** Upgrade `@` from simple keyword matching to proper RAG:
> embed the query using TF-IDF style bag-of-words, compute cosine similarity
> against pre-computed command vectors, retrieve top-K matches, and pass
> them as context to the LLM. No external ML libraries needed — pure C.

**Prompt to Claude Code:**
```
Create rag_retriever.c and rag_retriever.h for AiShell Week 9.

This upgrades the @ command's registry lookup from keyword matching
(Week 8) to a proper RAG (Retrieval-Augmented Generation) approach:
  1. At startup: build a TF-IDF vector for each command in commands.json
  2. At query time: build a vector for the user's query
  3. Compute cosine similarity between query vector and all command vectors
  4. Return top-K most similar commands as context
  5. The @ handler passes this context to the LLM

This is a self-contained pure-C implementation — no Python, no numpy,
no external libraries. Uses bag-of-words with IDF weighting.

Requirements for rag_retriever.h:
  #define RAG_MAX_VOCAB    1024   // unique words across all commands
  #define RAG_MAX_DOCS     64     // max commands in registry
  #define RAG_TOP_K        3      // retrieve top 3 matches
  #define RAG_MIN_SCORE    0.1f   // minimum similarity threshold

  typedef struct {
    char   command_id[64];
    char   description[256];
    char   command[512];
    float  score;               // cosine similarity score
  } RagResult;

  int        rag_build_index(void);
  // builds vectors from current g_registry[] — call after registry_load()

  int        rag_query(const char *query, RagResult *results, int max_k);
  // returns number of results (0 to max_k), filled into results[]

  char      *rag_build_context(RagResult *results, int count);
  // builds a context string to inject into the LLM prompt
  // returns malloc'd string — caller must free()

Requirements for rag_retriever.c:

1. Vocabulary building (rag_build_index):
   - Tokenize every command's description + aliases + command string
     into lowercase words (split on space, punctuation)
   - Remove stop words: the, a, an, is, to, for, in, of, and, or, not
   - Build a global vocab[] array of unique words (max RAG_MAX_VOCAB)
   - For each command, count word frequencies → term frequency (TF)
   - Compute IDF for each vocab word:
       idf[w] = log( (1 + N) / (1 + df[w]) ) + 1
       where N = total documents, df[w] = documents containing word w
   - Compute TF-IDF vector for each command: tfidf[d][w] = tf * idf[w]
   - L2-normalize each document vector (divide by its magnitude)
   - Store in a global float doc_vectors[RAG_MAX_DOCS][RAG_MAX_VOCAB]

2. Query processing (rag_query):
   - Tokenize the query, remove stop words, lowercase
   - Build query TF-IDF vector using the SAME vocab and IDF values
   - L2-normalize the query vector
   - For each document: cosine_sim = dot(query_vec, doc_vec)
     (since both are L2-normalized, cosine sim = dot product)
   - Sort by score descending
   - Return top min(max_k, count_above_threshold) results

3. Context building (rag_build_context):
   Build a string like:
   "Relevant commands from registry:\n"
   "1. [find_large_files] Find the 20 largest files: find / -xdev -type f...\n"
   "2. [disk_usage] Show disk usage summary: du -sh /*...\n"
   "3. [list_processes] Show running processes: ps -ef\n"
   This gets prepended to the LLM system prompt in claude_client.c

4. Thread safety:
   The index is built once at startup (single-threaded).
   rag_query() is read-only after that — safe for concurrent @ calls.

Show rag_retriever.c and rag_retriever.h in full.
```

---

## STEP 6 — Upgrade the `@` Handler to Use RAG

> **Goal:** Replace the simple `registry_lookup()` keyword match in `bnfc_repl()`
> with the RAG retriever, and inject retrieved context into the LLM prompt.

**Prompt to Claude Code:**
```
Update the @ handler inside bnfc_repl() in aishell_main.c to use RAG.

Here is my current bnfc_repl() @ handler block: [paste the @ block]

Replace STEP B (Registry lookup) with the new RAG-powered lookup:

NEW STEP B — RAG lookup (replaces simple registry_lookup):

  // Build query vector and retrieve top-K matches
  RagResult rag_results[RAG_TOP_K];
  int rag_count = rag_query(query, rag_results, RAG_TOP_K);

  if (rag_count > 0 && rag_results[0].score >= 0.5f) {
    // High confidence match (score >= 0.5) — treat like a registry hit
    Print: "[rag] Top match (score=%.2f): <rag_results[0].description>"
    Print: "[command] <rag_results[0].command>"

    // Show alternatives if any
    for i in 1..rag_count:
      Print: "[rag] Also relevant: <rag_results[i].description> (%.2f)"

    // Same confirmation flow as Week 8 registry hit
    // destructive command check → y/N prompt → execute or cancel

  } else if (rag_count > 0) {
    // Low confidence — show matches as context, go to LLM anyway
    Print: "[rag] Low confidence matches — sending to AI with context..."
    // build context string and fall through to MCP/Claude step
    // but pass the context to claude_query()

  } else {
    Print: "[rag] No relevant commands found — sending to AI..."
    // fall through to MCP/Claude with no context
  }

Also update claude_client.c to accept an optional context string:
  Change: ClaudeResponse claude_query(const char *query)
  To:     ClaudeResponse claude_query(const char *query, const char *context)

  When context is not NULL, prepend it to the system prompt:
  system = context + "\n\nYou are a shell command expert. Given the above "
           "relevant commands and the user query, suggest the best shell "
           "command. Respond ONLY with JSON: "
           "{\"command\": \"...\", \"explanation\": \"...\"}"

  When context is NULL: use the original system prompt (backward compatible)

Also update the call sites in mcp_client.c if needed.

Show the updated @ block in bnfc_repl() and the updated claude_client.c.
```

---

## STEP 7 — Add `server` MCP Tool Definitions (JSON Schema)

> **Goal:** The MCP PPT requires that tools be exposed with name, description,
> parameters (JSON Schema), and output format. Add this to the server.

**Prompt to Claude Code:**
```
Add formal MCP tool definitions to mcp_server.c following the MCP specification
from the ModelContextProtocol slides.

MCP requires each tool to have:
  - name: string
  - description: string
  - parameters: JSON Schema object
  - output_format: string

Implement a list_tools handler that returns all available tools:

static const char *MCP_TOOLS_JSON = 
"["
"  {"
"    \"name\": \"run_command\","
"    \"description\": \"Run an allowlisted AiShell built-in command\","
"    \"parameters\": {"
"      \"type\": \"object\","
"      \"properties\": {"
"        \"command\": {\"type\": \"string\", \"description\": \"Command name\"},"
"        \"args\":    {\"type\": \"string\", \"description\": \"Arguments\"}"
"      },"
"      \"required\": [\"command\"]"
"    },"
"    \"output_format\": \"text\""
"  },"
"  {"
"    \"name\": \"list_tools\","
"    \"description\": \"List all available MCP tools\","
"    \"parameters\": {\"type\": \"object\", \"properties\": {}},"
"    \"output_format\": \"json\""
"  },"
"  {"
"    \"name\": \"get_status\","
"    \"description\": \"Get AiShell server status and metrics\","
"    \"parameters\": {\"type\": \"object\", \"properties\": {}},"
"    \"output_format\": \"json\""
"  },"
"  {"
"    \"name\": \"get_registry\","
"    \"description\": \"Get all registered commands with descriptions\","
"    \"parameters\": {\"type\": \"object\", \"properties\": {}},"
"    \"output_format\": \"json\""
"  }"
"]";

Also implement get_status tool:
  Returns JSON with:
  {
    "status": "running",
    "server_port": 9000,
    "uptime_seconds": <seconds since start>,
    "connected_clients": <current count>,
    "total_requests": <lifetime count>,
    "registry_commands": <count from g_registry>,
    "log_file": "aishell_calls.log"
  }

And get_registry tool:
  Returns the same output as ./aishell --commands-json
  (call for_each_command() and build the JSON array)

Security: all tool handlers validate that the fd is a local connection
  (getsockname/getpeername check that peer is 127.0.0.1).
  If peer is not localhost: close immediately, log the attempt.

Show the updated handle_mcp_json() with all 4 tool handlers.
```

---

## STEP 8 — Configuration File (`aishell.conf`)

> **Goal:** The FTP slides explicitly require a config mechanism. Build it
> so the server can be enabled/disabled and limits can be set without recompiling.

**Prompt to Claude Code:**
```
Create a complete aishell.conf and the config.c / config.h module.

aishell.conf (default values):
  # AiShell Configuration File
  # Week 9 — MCP Server + FTP Service

  # Server settings
  server_enabled=1
  server_port=9000
  server_max_clients=16
  server_timeout=30

  # Allowlisted commands (comma-separated, no spaces)
  # Only these commands can be executed via MCP run_command tool
  server_allowlist=ls,cat,grep,echo,find,du,df,ps,wc,sort,uniq,cut,tr,head,tail

  # RAG settings
  rag_enabled=1
  rag_top_k=3
  rag_min_score=0.1

  # Logging
  log_file=aishell_calls.log
  log_mcp_calls=1

  # OpenRouter
  # api_key is read from OPENROUTER_API_KEY environment variable
  ai_model=meta-llama/llama-3.1-8b-instruct:free

Requirements for config.c:

1. config_load(path):
   - Open file, read line by line
   - Skip lines starting with # (comments) or empty lines
   - Parse "key=value" format
   - Populate g_config struct
   - If file not found: use defaults, create aishell.conf with defaults

2. config_save(path):
   - Write current g_config back to file with comments

3. The "server config" command (in lsh_server):
   server config             → print all settings
   server config key=value   → update one setting and save
   Examples:
     server config server_enabled=0   → disable server
     server config server_timeout=60  → change timeout
     server config server_allowlist=ls,cat,grep

4. Validation in config_load:
   - server_port must be 1024-65535
   - server_max_clients must be 1-64
   - server_timeout must be 5-300

Show config.c, config.h, and the default aishell.conf file.
```

---

## STEP 9 — Input Validation and Safety Hardening

> **Goal:** The FTP acceptance checks explicitly require "input validation
> prevents crashes on malformed requests." Harden all new server code.

**Prompt to Claude Code:**
```
Review and harden all Week 9 server code for the FTP acceptance checks.

The acceptance criteria state:
- Server accepts a connection, serves at least one request, and closes cleanly
- Timeouts prevent hanging connections
- Configuration changes behavior (enable/disable service or adjust limits)
- Input validation prevents crashes on malformed requests

Please audit ftp_handler.c and mcp_server.c for these specific cases:

FTP handler hardening:
1. NUL bytes in filenames: reject any filename containing \0 in the name
   (NUL bytes in file CONTENT are fine — use read()/write() not str functions)
2. Directory traversal: reject any path containing ".." component
3. Very long filenames: reject if strlen(filename) > 255
4. Very long command lines: reject if line > 1024 bytes (send 500 error)
5. PORT command validation:
   - All 6 numbers must be 0-255
   - Resulting port must be > 1023 (reject ports 0-1023)
   - IP must be overridden to 127.0.0.1 regardless of what client sends
6. STOR with no preceding PORT: send "425 Use PORT first.\r\n"
7. Commands before USER: send "530 Please login with USER.\r\n"
8. Simulate timeout: if client sends nothing for MCP_RECV_TIMEOUT seconds,
   close the connection (setsockopt SO_RCVTIMEO already set — verify it works)

MCP JSON handler hardening:
9. Malformed JSON (missing braces, no "tool" field): return error JSON,
   do not crash
10. run_command with empty command: return error
11. run_command with command not in allowlist: return error (already done
    in Step 2 — verify the check is working)
12. Extremely long "query" parameter (> 4096 bytes): truncate to 4096

General server hardening:
13. accept() loop: if accept() returns -1 with errno=EINTR, continue
    (don't exit the server)
14. If a client pthread crashes (test with pthread_cancel): the server
    loop must continue serving other clients — verify this is the case
    since each client runs in its own detached pthread

For each issue, show the before and after code fix.
```

---

## STEP 10 — Demo Script and Work Log

> **Goal:** The assignment requires "demo artifact + AI work log + verification steps."

**Prompt to Claude Code:**
```
Generate the Week 9 demo artifacts.

1. Create test_week9_demo.sh:

DEMO 1 — FTP server basic connection:
  Start aishell in background
  Run: ftp 127.0.0.1 9000 << EOF
    USER testuser
    PWD
    QUIT
  EOF
  Expected: "220 AiShell FTP Service ready", "230 User testuser logged in"
  PASS if both strings appear in output

DEMO 2 — FTP MKD + LIST:
  ftp 127.0.0.1 9000 << EOF
    USER test
    MKD testdir_week9
    LIST
    QUIT
  EOF
  Expected: "257 directory created", ls output shows testdir_week9
  PASS if directory was created and appears in LIST

DEMO 3 — FTP STOR + RETR (with NUL bytes):
  Create test file: printf "hello\x00world\n" > /tmp/nul_test.bin
  ftp STOR /tmp/nul_test.bin as nul_test.bin
  ftp RETR nul_test.bin as /tmp/retrieved.bin
  Expected: files are binary-identical (cmp /tmp/nul_test.bin /tmp/retrieved.bin)
  PASS if cmp exits 0

DEMO 4 — MCP JSON tool call:
  echo '{"type":"mcp","tool":"list_tools","params":{}}' | nc 127.0.0.1 9000
  Expected: JSON response containing "run_command" and "list_tools"
  PASS if response contains both tool names

DEMO 5 — RAG @ query:
  echo '@ find large files' | ./aishell
  Expected: [rag] Top match with score >= 0.5
  PASS if score is shown and command is suggested

DEMO 6 — Config: disable and re-enable server:
  ./aishell << EOF
    server config server_enabled=0
    server stop
    server status
    server config server_enabled=1
    server start
    server status
  EOF
  Expected: "stopped" then "running"

DEMO 7 — Input validation:
  ftp 127.0.0.1 9000 << EOF
    USER test
    RETR ../../../etc/passwd
    RETR nonexistent_file.txt
    STOR
    QUIT
  EOF
  Expected: "550 Permission denied" for traversal, "550 File not found",
            "500 Syntax error" for empty STOR

DEMO 8 — Regression: Week 7/8 still works:
  bash test_week7.sh | tail -1
  bash test_week8.sh | tail -1
  Expected: both show all tests passing

Each demo prints === DEMO N === header, runs the test, prints PASS or FAIL.

2. Create WEEK9_WORKLOG.md:

  ## Week 9 AI Work Log — MCP Server + FTP + RAG

  ### Architecture implemented
  | Component | Description |
  |---|---|
  | mcp_server.c | TCP server on port 9000, pthread per client |
  | ftp_handler.c | FTP protocol: USER/QUIT/PORT/STOR/RETR/LIST/MKD |
  | rag_retriever.c | TF-IDF vectors + cosine similarity for @ command |
  | config.c | aishell.conf loader + server config command |

  ### Files created this week
  [table of all new files]

  ### What I asked Claude Code to generate
  [fill in]

  ### What I changed manually and why
  [fill in]

  ### Demo results
  [fill in after running test_week9_demo.sh]

  ### Known limitations
  - FTP data connection uses passive client-specified port (active mode only)
  - RAG uses TF-IDF not neural embeddings — semantic similarity is approximate
  - MCP server handles one request per FTP session, then closes
  - No TLS — teaching example only

  ### Next week (Session 10)
  - Agentic RAG loop (iterative retrieval + retry)
  - Graph RAG vs vector search
  - Multi-step AI decisions via MCP

Show test_week9_demo.sh and WEEK9_WORKLOG.md in full.
```

---

## STEP 11 — Final Build Check

**Prompt to Claude Code:**
```
Do a final build check for the complete Week 9 AiShell project.

File list:
  aishell_main.c
  bnfc/Grammar.cf + generated files
  config.c / config.h
  mcp_server.c / mcp_server.h
  ftp_handler.c / ftp_handler.h
  rag_retriever.c / rag_retriever.h
  cmd_registry.c / cmd_registry.h     (Week 8)
  mcp_client.c / mcp_client.h         (Week 8)
  claude_client.c / claude_client.h   (Week 8, updated in Step 6)
  aishell_log.c / aishell_log.h       (Week 8)
  cJSON.c / cJSON.h                   (if used)
  commands.json
  aishell.conf
  Makefile

Please:

1. Write the final clean Makefile preserving ALL Week 7 BNFC targets:
   CC      = gcc
   CFLAGS  = -Wall -Wextra -g -pthread -DUSE_BNFC -Ibnfc
   SRCS    = aishell_main.c \
             config.c mcp_server.c ftp_handler.c rag_retriever.c \
             cmd_registry.c mcp_client.c claude_client.c aishell_log.c \
             [all cmd_*.c files from improvements]
   BNFC_SRCS = bnfc/Parser.c bnfc/Lexer.c bnfc/Absyn.c \
               bnfc/Printer.c bnfc/Buffer.c
   LIBS    = -lpthread -lm \
             $(shell pkg-config --libs libcurl 2>/dev/null) \
             $(shell pkg-config --libs libreadline 2>/dev/null)
   TARGET  = aishell

   Note: -lm is needed for log() in rag_retriever.c (IDF calculation)

2. Verify the Week 7 build sequence still works unchanged:
   make bnfc-gen
   cd bnfc && bison -t -pgrammar_ Grammar.y -o Parser.c
   cd bnfc && flex -Pgrammar_ -oLexer.c Grammar.l && cd ..
   make

3. New Makefile targets:
   make run      → ./aishell
   make test9    → bash test_week9_demo.sh
   make testall  → bash test_week5.sh && bash test_week6.sh &&
                   bash test_week7.sh && bash test_week8.sh &&
                   bash test_week9_demo.sh
   make log      → tail -20 aishell_calls.log
   make ftp-test → ftp 127.0.0.1 9000

4. Architecture comment block for top of aishell_main.c (update existing):
   /*
    * AiShell — Week 9 Milestone
    *
    * Parser:    BNFC grammar → LR parser → typed AST           (Week 7)
    * Input:     preprocess_arith → preprocess_quotes → psInput (Week 7)
    *
    * Command execution:
    *   External   → fork/exec                                  (Week 5)
    *   Built-ins  → pthread                                    (Week 6)
    *   @ queries  → RAG retrieval → MCP client → LLM           (Week 8/9)
    *
    * MCP Server (port 9000):
    *   FTP protocol  → USER/QUIT/PORT/STOR/RETR/LIST/MKD       (Week 9)
    *   MCP JSON tools → run_command/list_tools/get_status       (Week 9)
    *   Config         → aishell.conf                            (Week 9)
    *
    * RAG pipeline:
    *   TF-IDF index → cosine similarity → top-K context → LLM  (Week 9)
    *
    * All calls logged to aishell_calls.log
    */

Show the final Makefile and updated architecture comment.
```

---

## Week 8 → Week 9 Change Summary

| Component | Week 8 | Week 9 |
|---|---|---|
| `@` registry lookup | Keyword match in commands.json | TF-IDF cosine similarity (RAG) |
| `@` LLM prompt | Query only | Query + retrieved context |
| Socket role | Client only (MCP client) | Client + **Server** (port 9000) |
| Protocol | MCP JSON client | FTP-style server + MCP JSON server |
| Concurrency | pthreads for built-ins | + pthreads per FTP/MCP client |
| Config | Environment vars only | `aishell.conf` + `server config` command |
| Tools exposed | None | `run_command`, `list_tools`, `get_status`, `get_registry` |
| Data transfer | None | Binary-safe STOR/RETR (NUL-safe) |

---

## Acceptance Checklist (FTP Slides + MCP Slides)

**FTP/Server (from FTP_Service.pptx):**
- [ ] `ftp 127.0.0.1 9000` connects and gets "220 ready" greeting
- [ ] USER command logs in, sets home as working directory
- [ ] MKD creates directory; second MKD same name returns 550 error
- [ ] STOR stores a file including NUL bytes (binary safe)
- [ ] RETR retrieves a file including NUL bytes (binary safe)
- [ ] RETR nonexistent file returns 550
- [ ] LIST returns ls -l output over data connection
- [ ] QUIT closes only the connection, server keeps running
- [ ] Reconnect after QUIT works without restarting server
- [ ] PATH traversal (../etc/passwd) returns 550 Permission denied
- [ ] Timeout: idle client disconnected after server_timeout seconds

**MCP Tools (from ModelContextProtocol.pptx):**
- [ ] `list_tools` returns JSON with all 4 tool definitions + JSON schema
- [ ] `run_command` with allowlisted command returns output
- [ ] `run_command` with non-allowlisted command returns error
- [ ] `get_status` returns JSON with port, uptime, client count
- [ ] `get_registry` returns same JSON as `--commands-json`
- [ ] Non-localhost connection rejected immediately

**RAG @ (from RAG_EdgeAI.pptx):**
- [ ] `@ find large files` returns [rag] score >= 0.5 and correct command
- [ ] `@ unknown query` falls through to LLM with retrieved context
- [ ] LLM prompt includes retrieved command context (verify in log)
- [ ] `@ list` still works (no regression)

**Config:**
- [ ] `server config server_enabled=0` stops server from accepting connections
- [ ] `server config server_timeout=5` changes timeout (verify with idle test)
- [ ] aishell.conf created with defaults if not found

**Regression:**
- [ ] `test_week8.sh` — all pass
- [ ] `test_week7.sh` — 26/26
- [ ] `test_week6.sh` — 23/23
- [ ] `test_week5.sh` — 25/25

---

*Week 9 of AiShell — MCP Server + FTP Protocol + RAG-Grounded @ Command*

