#pragma once

typedef struct {
    int  success;               /* 1 = got a suggestion, 0 = error        */
    char suggestion[2048];      /* shell command suggested by OpenRouter   */
    char explanation[2048];     /* one-line explanation                    */
    char error_msg[256];        /* human-readable error when success == 0 */
} AIResponse;

/* Send a natural-language query to OpenRouter (multi-model fallback chain)
 * and return a shell command suggestion.
 *
 * Requires:  OPENROUTER_API_KEY environment variable
 *            libcurl dev headers at compile time  (-DHAVE_CURL -lcurl)
 *
 * When built WITHOUT -DHAVE_CURL the function returns success=0 and
 * prints an installation hint — no crash, graceful degradation.         */
AIResponse openrouter_query(const char *query);

/* Context-aware variant: RAG context is prepended to the system prompt.
 * Pass context=NULL to behave identically to openrouter_query(). */
AIResponse openrouter_query_ctx(const char *query, const char *context);
