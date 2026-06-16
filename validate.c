/* validate.c — Single-line + catalog-only validation for @ suggestions.
 * Week 10: every suggestion from RAG, MCP, OpenRouter, or heuristic
 * must pass through validate_suggestion() before being shown to the user.
 */
#define _POSIX_C_SOURCE 200809L
#include <string.h>
#include <ctype.h>

#include "validate.h"
#include "cmd_spec.h"

ValidatedSuggestion validate_suggestion(const char *raw_suggestion) {
    ValidatedSuggestion v;
    memset(&v, 0, sizeof(v));

    if (!raw_suggestion || !*raw_suggestion) {
        v.status = SUGGEST_EMPTY;
        return v;
    }

    /* ── 1. Copy and truncate at first newline ─────────────────────── */
    strncpy(v.line, raw_suggestion, sizeof(v.line) - 1);
    v.line[sizeof(v.line) - 1] = '\0';
    char *nl = strchr(v.line, '\n');
    if (nl) {
        *nl = '\0';
        v.status = SUGGEST_MULTILINE_TRUNCATED;
    }

    /* ── 2. Trim leading whitespace ────────────────────────────────── */
    char *start = v.line;
    while (*start == ' ' || *start == '\t') start++;
    if (start != v.line)
        memmove(v.line, start, strlen(start) + 1);

    /* ── Trim trailing whitespace ──────────────────────────────────── */
    size_t len = strlen(v.line);
    while (len > 0 && (v.line[len - 1] == ' ' || v.line[len - 1] == '\t'))
        v.line[--len] = '\0';

    /* ── 3. Empty after trim ───────────────────────────────────────── */
    if (len == 0) {
        v.status = SUGGEST_EMPTY;
        return v;
    }

    /* ── 4. Extract first whitespace token ─────────────────────────── */
    size_t i = 0;
    while (i < len && v.line[i] != ' ' && v.line[i] != '\t' &&
           i < sizeof(v.command_name) - 1) {
        v.command_name[i] = v.line[i];
        i++;
    }
    v.command_name[i] = '\0';

    /* Strip leading path prefix: "/bin/ls" → "ls" */
    const char *base = strrchr(v.command_name, '/');
    if (base && *(base + 1) != '\0')
        memmove(v.command_name, base + 1, strlen(base + 1) + 1);

    /* ── 5 & 6. Catalog check against cmd_spec registry ───────────── */
    if (find_command(v.command_name) != NULL) {
        /* Command found — keep MULTILINE_TRUNCATED if already set */
        if (v.status != SUGGEST_MULTILINE_TRUNCATED)
            v.status = SUGGEST_OK;
    } else {
        v.status = SUGGEST_UNKNOWN_COMMAND;
    }

    return v;
}
