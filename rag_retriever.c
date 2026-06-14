/* rag_retriever.c — TF-IDF retrieval for the @ command. Week 9.
 *
 * At startup: rag_build_index() tokenizes every command's description +
 * aliases into a bag-of-words TF-IDF vector.  At query time: build the
 * query vector using the same vocab + IDF weights, compute cosine
 * similarities, return top-K matches above the minimum threshold.
 *
 * No external ML libraries — pure C, requires -lm for log()/sqrt().
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "rag_retriever.h"
#include "cmd_registry.h"

/* ── Vocabulary ─────────────────────────────────────────────────────*/
static char vocab[RAG_MAX_VOCAB][64];
static int  vocab_size = 0;

/* ── Per-document data ──────────────────────────────────────────────*/
static char doc_id[RAG_MAX_DOCS][64];
static char doc_desc[RAG_MAX_DOCS][256];
static char doc_cmd[RAG_MAX_DOCS][512];
static int  doc_count = 0;

/* ── TF-IDF matrices (static — built once, read-only afterwards) ────*/
static float raw_tf[RAG_MAX_DOCS][RAG_MAX_VOCAB];   /* term frequency counts */
static float idf[RAG_MAX_VOCAB];                    /* inverse doc frequency */
static float doc_vec[RAG_MAX_DOCS][RAG_MAX_VOCAB];  /* TF-IDF, L2-normalised */
static int   doc_freq[RAG_MAX_VOCAB];               /* # docs containing word */

/* ── Stop words ─────────────────────────────────────────────────────*/
static const char * const STOPWORDS[] = {
    "the","a","an","is","to","for","in","of","and","or","not",
    "with","from","by","on","at","its","this","that","all",
    NULL
};

static int is_stopword(const char *w) {
    for (int i = 0; STOPWORDS[i]; i++)
        if (!strcmp(w, STOPWORDS[i])) return 1;
    return 0;
}

/* ── Vocab helpers ──────────────────────────────────────────────────*/
static int vocab_find_or_add(const char *w) {
    for (int i = 0; i < vocab_size; i++)
        if (!strcmp(vocab[i], w)) return i;
    if (vocab_size >= RAG_MAX_VOCAB) return -1;
    snprintf(vocab[vocab_size], sizeof(vocab[0]), "%s", w);
    return vocab_size++;
}

static int vocab_find(const char *w) {
    for (int i = 0; i < vocab_size; i++)
        if (!strcmp(vocab[i], w)) return i;
    return -1;
}

/* ── Tokenize text into the TF row for a document ──────────────────*/
static void tokenize_into_tf(const char *text, float *tf_row) {
    char buf[128];
    size_t bi = 0;
    for (const char *p = text; ; p++) {
        if (isalpha((unsigned char)*p) || *p == '_') {
            if (bi < sizeof(buf) - 1)
                buf[bi++] = (char)tolower((unsigned char)*p);
        } else {
            if (bi > 0) {
                buf[bi] = '\0';
                if (!is_stopword(buf)) {
                    int wi = vocab_find_or_add(buf);
                    if (wi >= 0) tf_row[wi] += 1.0f;
                }
                bi = 0;
            }
            if (!*p) break;
        }
    }
}

/* ── Tokenize query into an already-built-vocab query vector ────────*/
static void tokenize_query(const char *text, float *qv) {
    char buf[128];
    size_t bi = 0;
    for (const char *p = text; ; p++) {
        if (isalpha((unsigned char)*p) || *p == '_') {
            if (bi < sizeof(buf) - 1)
                buf[bi++] = (char)tolower((unsigned char)*p);
        } else {
            if (bi > 0) {
                buf[bi] = '\0';
                if (!is_stopword(buf)) {
                    int wi = vocab_find(buf); /* read-only — no new words */
                    if (wi >= 0) qv[wi] += 1.0f;
                }
                bi = 0;
            }
            if (!*p) break;
        }
    }
}

/* ── Index-build registry callback ──────────────────────────────────*/
static int g_build_idx = 0;  /* incremented per entry during build */

static void index_entry_cb(const RegistryEntry *e, void *ud) {
    (void)ud;
    int d = g_build_idx;
    if (d >= RAG_MAX_DOCS) return;

    snprintf(doc_id[d],   sizeof(doc_id[d]),   "%s", e->id);
    snprintf(doc_desc[d], sizeof(doc_desc[d]), "%s", e->description);
    snprintf(doc_cmd[d],  sizeof(doc_cmd[d]),  "%s", e->command);

    tokenize_into_tf(e->description, raw_tf[d]);
    for (int j = 0; j < e->alias_count; j++)
        tokenize_into_tf(e->aliases[j], raw_tf[d]);
    tokenize_into_tf(e->id, raw_tf[d]);
    tokenize_into_tf(e->command, raw_tf[d]);

    g_build_idx++;
}

/* ── Public: build index ─────────────────────────────────────────── */
int rag_build_index(void) {
    vocab_size  = 0;
    doc_count   = 0;
    g_build_idx = 0;
    memset(raw_tf,    0, sizeof(raw_tf));
    memset(idf,       0, sizeof(idf));
    memset(doc_vec,   0, sizeof(doc_vec));
    memset(doc_freq,  0, sizeof(doc_freq));

    registry_for_each(index_entry_cb, NULL);
    doc_count = g_build_idx;

    if (doc_count == 0 || vocab_size == 0) return 0;

    /* Document frequency: how many docs contain each word */
    for (int d = 0; d < doc_count; d++)
        for (int w = 0; w < vocab_size; w++)
            if (raw_tf[d][w] > 0.0f) doc_freq[w]++;

    /* IDF = log((1 + N) / (1 + df(w))) + 1 */
    for (int w = 0; w < vocab_size; w++)
        idf[w] = logf((1.0f + (float)doc_count) /
                      (1.0f + (float)doc_freq[w])) + 1.0f;

    /* Compute TF-IDF and L2-normalise each document vector */
    for (int d = 0; d < doc_count; d++) {
        float mag = 0.0f;
        for (int w = 0; w < vocab_size; w++) {
            doc_vec[d][w] = raw_tf[d][w] * idf[w];
            mag += doc_vec[d][w] * doc_vec[d][w];
        }
        mag = sqrtf(mag);
        if (mag > 1e-9f)
            for (int w = 0; w < vocab_size; w++)
                doc_vec[d][w] /= mag;
    }

    return doc_count;
}

/* ── Public: query ──────────────────────────────────────────────────*/
int rag_query(const char *query, RagResult *results, int max_k) {
    if (!query || doc_count == 0 || vocab_size == 0) return 0;

    /* Build query TF-IDF vector (aligned to existing vocab — no additions) */
    float qv[RAG_MAX_VOCAB];
    memset(qv, 0, sizeof(qv));
    tokenize_query(query, qv);

    /* Apply IDF weights and L2-normalise */
    float qmag = 0.0f;
    for (int w = 0; w < vocab_size; w++) {
        qv[w] *= idf[w];
        qmag  += qv[w] * qv[w];
    }
    qmag = sqrtf(qmag);
    if (qmag < 1e-9f) return 0; /* none of the query words are in vocab */
    for (int w = 0; w < vocab_size; w++)
        qv[w] /= qmag;

    /* Cosine similarity = dot product (both vectors are already L2-normalised) */
    float scores[RAG_MAX_DOCS];
    for (int d = 0; d < doc_count; d++) {
        float dot = 0.0f;
        for (int w = 0; w < vocab_size; w++)
            dot += qv[w] * doc_vec[d][w];
        scores[d] = dot;
    }

    /* Select top max_k indices by partial selection sort */
    int order[RAG_MAX_DOCS];
    for (int i = 0; i < doc_count; i++) order[i] = i;
    int k = (max_k < doc_count) ? max_k : doc_count;
    for (int i = 0; i < k; i++) {
        for (int j = i + 1; j < doc_count; j++) {
            if (scores[order[j]] > scores[order[i]]) {
                int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
            }
        }
    }

    int found = 0;
    for (int i = 0; i < k && found < max_k; i++) {
        int d = order[i];
        if (scores[d] < RAG_MIN_SCORE) break;
        RagResult *r = &results[found++];
        snprintf(r->command_id,  sizeof(r->command_id),  "%s", doc_id[d]);
        snprintf(r->description, sizeof(r->description), "%s", doc_desc[d]);
        snprintf(r->command,     sizeof(r->command),     "%s", doc_cmd[d]);
        r->score = scores[d];
    }
    return found;
}

/* ── Public: build context string ───────────────────────────────────*/
char *rag_build_context(RagResult *results, int count) {
    if (count <= 0) return NULL;

    char *buf = malloc(4096);
    if (!buf) return NULL;

    int off = snprintf(buf, 4096,
        "The following shell commands are relevant to the user's request.\n"
        "Use the 'shell command' value as the command field in your JSON response.\n\n");
    for (int i = 0; i < count && off < 3800; i++) {
        off += snprintf(buf + off, 4096 - (size_t)off,
                        "%d. Task: %s\n"
                        "   Shell command to run: %s\n\n",
                        i + 1,
                        results[i].description,
                        results[i].command);
    }
    return buf;
}
