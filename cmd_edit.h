#ifndef CMD_EDIT_H
#define CMD_EDIT_H

#include <stddef.h>

/* ── Core edit operations ───────────────────────────────────────────────
 * All functions return 0 on success, -1 on failure.
 * On failure, a human-readable message is written into errbuf[0..errsz-1].
 * On success, errbuf[0] is '\0'.
 *
 * Used by cmd_edit.c (the `edit` built-in) and mcp_server.c (MCP wrappers).
 * ──────────────────────────────────────────────────────────────────────── */

/* Replace line linenum (1-indexed) with text (newline appended if missing). */
int edit_op_replace_line(const char *path, int linenum, const char *text,
                         char *errbuf, size_t errsz);

/* Insert text BEFORE line linenum (linenum == count+1 appends at end). */
int edit_op_insert_line(const char *path, int linenum, const char *text,
                        char *errbuf, size_t errsz);

/* Delete line linenum from path. */
int edit_op_delete_line(const char *path, int linenum,
                        char *errbuf, size_t errsz);

/* Global literal-string replace: replace all occurrences of pattern with
 * replacement.  Reports the number of replacements via *nreplaced. */
int edit_op_replace(const char *path, const char *pattern,
                    const char *replacement, int *nreplaced,
                    char *errbuf, size_t errsz);

/* Register the unified `edit` command with the shell registry. */
void register_edit_command(void);

#endif /* CMD_EDIT_H */
