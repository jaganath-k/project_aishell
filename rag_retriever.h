#pragma once

#define RAG_MAX_VOCAB   1024   /* unique words across all commands */
#define RAG_MAX_DOCS      64   /* max commands in registry */
#define RAG_TOP_K          3   /* retrieve top 3 matches by default */
#define RAG_MIN_SCORE   0.1f   /* minimum cosine similarity threshold */

typedef struct {
    char  command_id[64];    /* registry entry id */
    char  description[256];  /* entry description */
    char  command[512];      /* suggested shell command */
    float score;             /* cosine similarity [0,1] */
} RagResult;

/* Build TF-IDF vectors from the current loaded registry.
 * Call once after registry_load().  Returns number of docs indexed. */
int   rag_build_index(void);

/* Query: fill results[] with up to max_k best-matching commands.
 * Returns the count of results placed (0 if nothing above threshold). */
int   rag_query(const char *query, RagResult *results, int max_k);

/* Build a context string from retrieved results for injection into the LLM prompt.
 * Returns malloc'd string — caller must free(). */
char *rag_build_context(RagResult *results, int count);

/* Deterministic heuristic fallback (port of mysh_llm.py score_command).
 * Scores every registered cmd_spec_t by keyword matching (name=3,
 * summary=2, long_help=1) and returns the best match's name.
 * Returns malloc'd string — caller must free(). Never NULL if registry
 * has at least one command. No network calls — pure local computation. */
char *heuristic_fallback_command(const char *query);
