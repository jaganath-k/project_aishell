/* config.c — aishell.conf key=value config loader / saver. Week 9. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include "config.h"

/* Default configuration */
AiShellConfig g_config = {
    .server_enabled    = 1,
    .server_port       = 9000,
    .server_max_clients = 16,
    .server_timeout    = 30,
    .server_allowlist  = "ls,cat,grep,echo,find,du,df,ps,wc,sort,uniq,cut,tr,head,tail",
    .rag_enabled       = 1,
    .rag_top_k         = 3,
    .rag_min_score     = 0.1f,
    .log_file          = "aishell_calls.log",
    .log_mcp_calls     = 1,
    .ai_model          = "meta-llama/llama-3.1-8b-instruct:free",
};

static void apply_kv(const char *key, const char *val) {
    if      (!strcmp(key,"server_enabled"))      g_config.server_enabled     = atoi(val);
    else if (!strcmp(key,"server_port")) {
        int p = atoi(val);
        if (p >= 1024 && p <= 65535) g_config.server_port = p;
    }
    else if (!strcmp(key,"server_max_clients")) {
        int m = atoi(val);
        if (m >= 1 && m <= 64) g_config.server_max_clients = m;
    }
    else if (!strcmp(key,"server_timeout")) {
        int t = atoi(val);
        if (t >= 5 && t <= 300) g_config.server_timeout = t;
    }
    else if (!strcmp(key,"server_allowlist"))
        snprintf(g_config.server_allowlist, sizeof(g_config.server_allowlist), "%s", val);
    else if (!strcmp(key,"rag_enabled"))    g_config.rag_enabled    = atoi(val);
    else if (!strcmp(key,"rag_top_k")) {
        int k = atoi(val);
        if (k >= 1 && k <= 20) g_config.rag_top_k = k;
    }
    else if (!strcmp(key,"rag_min_score"))  g_config.rag_min_score  = (float)atof(val);
    else if (!strcmp(key,"log_file"))
        snprintf(g_config.log_file, sizeof(g_config.log_file), "%s", val);
    else if (!strcmp(key,"log_mcp_calls"))  g_config.log_mcp_calls  = atoi(val);
    else if (!strcmp(key,"ai_model"))
        snprintf(g_config.ai_model, sizeof(g_config.ai_model), "%s", val);
}

void config_load(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        /* Create with defaults if missing */
        config_save(path);
        return;
    }

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        /* Strip trailing newline */
        line[strcspn(line, "\r\n")] = '\0';

        /* Skip comments and blank lines */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0') continue;

        /* Split on first '=' */
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = p;
        const char *val = eq + 1;

        /* Trim key trailing whitespace */
        char *ke = (char *)key + strlen(key) - 1;
        while (ke > key && (*ke == ' ' || *ke == '\t')) { *ke = '\0'; ke--; }

        /* Trim val leading whitespace */
        while (*val == ' ' || *val == '\t') val++;

        apply_kv(key, val);
    }
    fclose(fp);
}

void config_save(const char *path) {
    FILE *fp = fopen(path, "w");
    if (!fp) { perror(path); return; }

    fprintf(fp,
        "# AiShell Configuration — Week 9: MCP Server + FTP Service\n"
        "# Edit and restart aishell, or use: server config key=value\n"
        "\n"
        "# Server settings\n"
        "server_enabled=%d\n"
        "server_port=%d\n"
        "server_max_clients=%d\n"
        "server_timeout=%d\n"
        "\n"
        "# Commands allowed via MCP run_command tool (comma-separated)\n"
        "server_allowlist=%s\n"
        "\n"
        "# RAG settings\n"
        "rag_enabled=%d\n"
        "rag_top_k=%d\n"
        "rag_min_score=%.2f\n"
        "\n"
        "# Logging\n"
        "log_file=%s\n"
        "log_mcp_calls=%d\n"
        "\n"
        "# AI model (api_key read from OPENROUTER_API_KEY env var)\n"
        "ai_model=%s\n",
        g_config.server_enabled,
        g_config.server_port,
        g_config.server_max_clients,
        g_config.server_timeout,
        g_config.server_allowlist,
        g_config.rag_enabled,
        g_config.rag_top_k,
        (double)g_config.rag_min_score,
        g_config.log_file,
        g_config.log_mcp_calls,
        g_config.ai_model);

    fclose(fp);
}

void config_print(void) {
    printf("--- AiShell Configuration ---\n");
    printf("server_enabled      = %d\n",   g_config.server_enabled);
    printf("server_port         = %d\n",   g_config.server_port);
    printf("server_max_clients  = %d\n",   g_config.server_max_clients);
    printf("server_timeout      = %d\n",   g_config.server_timeout);
    printf("server_allowlist    = %s\n",   g_config.server_allowlist);
    printf("rag_enabled         = %d\n",   g_config.rag_enabled);
    printf("rag_top_k           = %d\n",   g_config.rag_top_k);
    printf("rag_min_score       = %.2f\n", (double)g_config.rag_min_score);
    printf("log_file            = %s\n",   g_config.log_file);
    printf("log_mcp_calls       = %d\n",   g_config.log_mcp_calls);
    printf("ai_model            = %s\n",   g_config.ai_model);
    printf("-----------------------------\n");
}

int config_set(const char *path, const char *keyval) {
    if (!keyval) return -1;
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", keyval);
    char *eq = strchr(buf, '=');
    if (!eq) return -1;
    *eq = '\0';
    const char *key = buf;
    const char *val = eq + 1;

    /* Validate key is known by checking if apply_kv would match */
    static const char * const KEYS[] = {
        "server_enabled","server_port","server_max_clients","server_timeout",
        "server_allowlist","rag_enabled","rag_top_k","rag_min_score",
        "log_file","log_mcp_calls","ai_model", NULL
    };
    int found = 0;
    for (int i = 0; KEYS[i]; i++)
        if (!strcmp(key, KEYS[i])) { found = 1; break; }
    if (!found) { fprintf(stderr, "config: unknown key '%s'\n", key); return -1; }

    apply_kv(key, val);
    config_save(path);
    printf("config: %s = %s\n", key, val);
    return 0;
}
