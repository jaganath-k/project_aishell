#pragma once

typedef struct {
    int   server_enabled;         /* 1 = start MCP server at boot */
    int   server_port;            /* TCP listen port (default 9000) */
    int   server_max_clients;     /* max concurrent clients (default 16) */
    int   server_timeout;         /* idle timeout seconds (default 30) */
    char  server_allowlist[512];  /* comma-separated allowed commands */
    int   rag_enabled;            /* 1 = use RAG for @ command */
    int   rag_top_k;              /* number of RAG results (default 3) */
    float rag_min_score;          /* minimum cosine score (default 0.1) */
    char  log_file[256];          /* log path (default aishell_calls.log) */
    int   log_mcp_calls;          /* 1 = log MCP calls */
    char  ai_model[128];          /* AI model name for OpenRouter */
} AiShellConfig;

/* Global config instance — initialised with defaults by config_load(). */
extern AiShellConfig g_config;

/* Read aishell.conf.  Creates the file with defaults if not found. */
void config_load(const char *path);

/* Write current g_config back to the config file with comments. */
void config_save(const char *path);

/* Print all current settings to stdout. */
void config_print(void);

/* Update a single "key=value" string in g_config and save.
 * Returns 0 on success, -1 if key is unknown. */
int config_set(const char *path, const char *keyval);
