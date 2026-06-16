#pragma once
#include <stddef.h>

/* ── MCP proc.* accessors ───────────────────────────────────────────────
 * Public interface to the job table in aishell_main.c for mcp_server.c.
 * All three functions build a complete JSON response in the caller's buffer.
 * ──────────────────────────────────────────────────────────────────────── */

/* Fill buf with a JSON array of all active jobs.
 * Returns bytes written (like snprintf). */
int proc_list_json(char *buf, size_t bufsz);

/* Send signal_name (e.g. "SIGTERM", "SIGKILL", "9") to job job_id.
 * Fills resp with a JSON response object.
 * Returns 0 on success, -1 on error. */
int proc_kill_json(int job_id, const char *signal_name,
                   char *resp, size_t respsz);

/* Block until job job_id's process exits (or return stored result if already
 * done).  For thread jobs, returns an error immediately.
 * Fills resp with a JSON response object including exit_code.
 * Returns 0 on success, -1 on error. */
int proc_wait_json(int job_id, char *resp, size_t respsz);

/* ── env.* sync helper ──────────────────────────────────────────────────
 * Update the REPL's private var_store so $NAME expansion sees the value
 * set by env.set via setenv().  Calling setenv() alone is sufficient for
 * var_get()'s getenv() fallback, but an existing var_store entry would
 * shadow it — this keeps both stores consistent.
 * ──────────────────────────────────────────────────────────────────────── */
void mcp_sync_var(const char *name, const char *value);
