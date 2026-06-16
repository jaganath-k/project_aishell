#pragma once

typedef enum {
    SUGGEST_OK,                  /* command found in registry, single line */
    SUGGEST_EMPTY,               /* suggestion was empty after trimming */
    SUGGEST_UNKNOWN_COMMAND,     /* first token not in cmd_spec registry */
    SUGGEST_MULTILINE_TRUNCATED  /* multi-line input truncated to first line;
                                  * command_name IS in registry */
} SuggestStatus;

typedef struct {
    SuggestStatus status;
    char line[1024];        /* validated (possibly truncated) single line */
    char command_name[64];  /* first whitespace token, path prefix stripped */
} ValidatedSuggestion;

/* Validate and normalise a raw suggestion string.
 *
 * Steps (in order):
 *   1. Truncate to first '\n'  → SUGGEST_MULTILINE_TRUNCATED if changed
 *   2. Trim leading/trailing whitespace
 *   3. Empty result            → SUGGEST_EMPTY
 *   4. Extract first token, strip leading path ("/" prefix → basename)
 *   5. find_command(token)     → SUGGEST_UNKNOWN_COMMAND if not found
 *   6. Otherwise               → SUGGEST_OK (or keep MULTILINE_TRUNCATED)
 */
ValidatedSuggestion validate_suggestion(const char *raw_suggestion);
