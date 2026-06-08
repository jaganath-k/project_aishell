#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aishell_client.h"
#include "aishell_log.h"

/* ── libcurl implementation ───────────────────────────────────────────────── */

#ifdef HAVE_CURL

/* Model priority chain — free models first, cheapest paid last.
 * 404 / 429 / 503 automatically skip to the next entry.
 *
 * Cost reference (OpenRouter, per 1M input tokens):
 *   gemma-3-4b-it        $0.04   ← cheapest paid
 *   gemini-2.5-flash-lite $0.10  ← reliable paid fallback
 *
 * Run this to see current available models:
 *   curl -s https://openrouter.ai/api/v1/models \
 *     -H "Authorization: Bearer $OPENROUTER_API_KEY" | \
 *     python3 -c "import json,sys; [print(m['id']) for m in json.load(sys.stdin)['data']]"
 */
static const char *MODEL_PRIORITY[] = {
    "openrouter/free"
    "google/gemma-4-31b-it:free",              /* free — 31B, best quality  */
    "meta-llama/llama-3.3-70b-instruct:free",  /* free — 70B, high quality  */
    "meta-llama/llama-3.2-3b-instruct:free",   /* free — small, fast        */
    "google/gemma-3-4b-it",                    /* $0.04/1M — cheapest paid  */
    "google/gemini-2.5-flash-lite",            /* $0.10/1M — reliable       */
    NULL
};

#define OPENROUTER_URL "https://openrouter.ai/api/v1/chat/completions"

#include <curl/curl.h>

/* ── JSON helpers ─────────────────────────────────────────────────────────── */

static void json_escape(const char *src, char *dst, size_t dstsz) {
    size_t di = 0;
    for (const char *p = src; *p && di + 2 < dstsz; p++) {
        unsigned char c = (unsigned char)*p;
        if      (c == '"')  { dst[di++] = '\\'; dst[di++] = '"';  }
        else if (c == '\\') { dst[di++] = '\\'; dst[di++] = '\\'; }
        else if (c == '\n') { dst[di++] = '\\'; dst[di++] = 'n';  }
        else if (c == '\r') { dst[di++] = '\\'; dst[di++] = 'r';  }
        else if (c == '\t') { dst[di++] = '\\'; dst[di++] = 't';  }
        else if (c < 0x20)  { /* skip control chars */              }
        else                  { dst[di++] = (char)c;                }
    }
    dst[di] = '\0';
}

static int json_extract_str(const char *json, const char *key,
                             char *out, size_t outsz) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *pos = strstr(json, pat);
    if (!pos) {
        snprintf(pat, sizeof(pat), "\"%s\": \"", key);
        pos = strstr(json, pat);
        if (!pos) return 0;
    }
    const char *val = pos + strlen(pat);
    size_t i = 0;
    while (*val && i < outsz - 1) {
        if (*val == '\\' && *(val + 1)) {
            val++;
            switch (*val) {
            case '"':  out[i++] = '"';  break;
            case '\\': out[i++] = '\\'; break;
            case 'n':  out[i++] = '\n'; break;
            case 'r':  out[i++] = '\r'; break;
            case 't':  out[i++] = '\t'; break;
            default:   out[i++] = *val; break;
            }
        } else if (*val == '"') {
            break;
        } else {
            out[i++] = *val;
        }
        val++;
    }
    out[i] = '\0';
    return 1;
}

/* Extract choices[0].message.content from an OpenAI-compatible response. */
static int extract_choice_content(const char *body, char *out, size_t outsz) {
    const char *choices = strstr(body, "\"choices\"");
    if (!choices) return 0;
    return json_extract_str(choices, "content", out, outsz);
}

/* ── curl write callback ──────────────────────────────────────────────────── */

typedef struct {
    char  *data;
    size_t size;
} CurlBuf;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userp) {
    CurlBuf *buf = (CurlBuf *)userp;
    size_t   add = size * nmemb;
    char    *tmp = realloc(buf->data, buf->size + add + 1);
    if (!tmp) return 0;
    buf->data = tmp;
    memcpy(buf->data + buf->size, ptr, add);
    buf->size += add;
    buf->data[buf->size] = '\0';
    return add;
}

/* ── try_model ────────────────────────────────────────────────────────────── */

/* Attempt one model via OpenRouter.
 * Returns 1 on success (resp populated).
 * Returns 0 to signal "try next model":
 *   HTTP 404 (model gone), 429 (rate limited), 503 (unavailable),
 *   empty content, or parse failure. */
static int try_model(const char *api_key, const char *model,
                     const char *query, AIResponse *resp) {
    char safe_query[2048], safe_model[256];
    json_escape(query, safe_query, sizeof(safe_query));
    json_escape(model, safe_model, sizeof(safe_model));

    char body[4096];
    snprintf(body, sizeof(body),
        "{"
          "\"model\":\"%s\","
          "\"max_tokens\":1024,"
          "\"messages\":["
            "{\"role\":\"system\","
             "\"content\":\"You are a shell command expert. When given a natural "
                           "language request, respond with ONLY a JSON object in "
                           "this exact format: "
                           "{\\\"command\\\": \\\"<the shell command>\\\", "
                           "\\\"explanation\\\": \\\"<one sentence why>\\\"}. "
                           "Never add markdown, never add extra text.\"},"
            "{\"role\":\"user\",\"content\":\"%s\"}"
          "]"
        "}",
        safe_model, safe_query);

    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "HTTP-Referer: https://github.com/aishell");
    headers = curl_slist_append(headers, "X-Title: AiShell");

    CurlBuf buf  = { NULL, 0 };
    CURL   *curl = curl_easy_init();
    if (!curl) {
        curl_slist_free_all(headers);
        snprintf(resp->error_msg, sizeof(resp->error_msg),
                 "[%s] curl_easy_init failed", model);
        return 0;
    }

    curl_easy_setopt(curl, CURLOPT_URL,           OPENROUTER_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    CURLcode cc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    if (cc != CURLE_OK) {
        free(buf.data);
        snprintf(resp->error_msg, sizeof(resp->error_msg),
                 "[%s] curl error: %s", model, curl_easy_strerror(cc));
        return 0;
    }

    /* Model gone / rate limited / unavailable — skip to next */
    if (http_code == 404 || http_code == 429 || http_code == 503) {
        char api_err[128] = "";
        if (buf.data)
            json_extract_str(buf.data, "message", api_err, sizeof(api_err));
        free(buf.data);
        snprintf(resp->error_msg, sizeof(resp->error_msg),
                 "[%s] HTTP %ld%s%s — skipping",
                 model, http_code, api_err[0] ? ": " : "", api_err);
        return 0;
    }

    if (http_code != 200) {
        char api_err[128] = "";
        if (buf.data)
            json_extract_str(buf.data, "message", api_err, sizeof(api_err));
        free(buf.data);
        snprintf(resp->error_msg, sizeof(resp->error_msg),
                 "[%s] HTTP %ld%s%s", model, http_code,
                 api_err[0] ? ": " : "", api_err);
        return 0;
    }

    if (!buf.data) {
        snprintf(resp->error_msg, sizeof(resp->error_msg),
                 "[%s] empty response body", model);
        return 0;
    }

    char content_text[4096] = "";
    if (!extract_choice_content(buf.data, content_text, sizeof(content_text))) {
        free(buf.data);
        snprintf(resp->error_msg, sizeof(resp->error_msg),
                 "[%s] could not parse choices[0].message.content", model);
        return 0;
    }
    free(buf.data);

    if (content_text[0] == '\0') {
        snprintf(resp->error_msg, sizeof(resp->error_msg),
                 "[%s] empty content in response", model);
        return 0;
    }

    if (!json_extract_str(content_text, "command",
                          resp->suggestion, sizeof(resp->suggestion))) {
        strncpy(resp->suggestion, content_text, sizeof(resp->suggestion) - 1);
        resp->suggestion[sizeof(resp->suggestion) - 1] = '\0';
        snprintf(resp->explanation, sizeof(resp->explanation),
                 "(raw response — JSON format not followed)");
    } else {
        json_extract_str(content_text, "explanation",
                         resp->explanation, sizeof(resp->explanation));
    }

    resp->success = 1;
    return 1;
}

/* ── public API ───────────────────────────────────────────────────────────── */

AIResponse openrouter_query(const char *query) {
    AIResponse resp;
    memset(&resp, 0, sizeof(resp));

    if (!query || query[0] == '\0') {
        snprintf(resp.error_msg, sizeof(resp.error_msg), "empty query");
        aishell_log(LOG_AI, "", "", "error-empty-query");
        return resp;
    }

    const char *api_key = getenv("OPENROUTER_API_KEY");
    if (!api_key || api_key[0] == '\0')
        api_key = getenv("ANTHROPIC_API_KEY");
    if (!api_key || api_key[0] == '\0') {
        snprintf(resp.error_msg, sizeof(resp.error_msg),
            "OPENROUTER_API_KEY not set. "
            "Get a free key at https://openrouter.ai and "
            "export OPENROUTER_API_KEY=sk-or-...");
        aishell_log(LOG_AI, query, "", "error-no-api-key");
        return resp;
    }

    /* AISHELL_MODEL override — skip the priority loop */
    const char *env_model = getenv("AISHELL_MODEL");
    if (env_model && env_model[0] != '\0') {
        fprintf(stderr, "[openrouter] using model from AISHELL_MODEL: %s\n", env_model);
        if (try_model(api_key, env_model, query, &resp)) {
            aishell_log(LOG_AI, query, resp.suggestion, env_model);
            return resp;
        }
        fprintf(stderr, "[openrouter] AISHELL_MODEL failed: %s\n", resp.error_msg);
        aishell_log(LOG_AI, query, "", "error-env-model-failed");
        return resp;
    }

    /* Walk the priority chain */
    for (int i = 0; MODEL_PRIORITY[i] != NULL; i++) {
        fprintf(stderr, "[openrouter] trying model: %s\n", MODEL_PRIORITY[i]);
        if (try_model(api_key, MODEL_PRIORITY[i], query, &resp)) {
            aishell_log(LOG_AI, query, resp.suggestion, MODEL_PRIORITY[i]);
            return resp;
        }
        fprintf(stderr, "[openrouter] %s\n", resp.error_msg);
    }

    /* All models exhausted */
    snprintf(resp.error_msg, sizeof(resp.error_msg),
        "All models failed or rate limited. Check openrouter.ai/activity");
    fprintf(stderr, "[openrouter] %s\n", resp.error_msg);
    fprintf(stderr, "[openrouter] Tip: check remaining credits at "
                    "https://openrouter.ai/credits\n");
    fprintf(stderr, "[openrouter] Tip: see available free models at "
                    "https://openrouter.ai/models?q=:free\n");
    aishell_log(LOG_AI, query, "", "error-all-models-failed");
    return resp;
}

#else   /* ── stub when libcurl dev headers not available ── */

AIResponse openrouter_query(const char *query) {
    AIResponse resp;
    memset(&resp, 0, sizeof(resp));

    fprintf(stdout,
        "[openrouter] libcurl not available at compile time.\n"
        "[openrouter] Install headers with:\n"
        "[openrouter]   sudo apt install libcurl4-openssl-dev\n"
        "[openrouter] Then rebuild:  make clean && make\n");

    snprintf(resp.error_msg, sizeof(resp.error_msg),
             "OpenRouter requires libcurl (rebuild with -DHAVE_CURL -lcurl)");

    aishell_log(LOG_AI, query ? query : "", "", "error-no-libcurl");
    return resp;
}

#endif /* HAVE_CURL */
