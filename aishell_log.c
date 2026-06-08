#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include "aishell_log.h"

#define LOG_FILE "aishell_calls.log"

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *source_name(LogSource src) {
    switch (src) {
    case LOG_REGISTRY: return "registry";
    case LOG_MCP:      return "mcp";
    case LOG_AI:       return "openrouter";
    case LOG_EXEC:     return "exec";
    default:           return "unknown";
    }
}

void aishell_log(LogSource source, const char *query,
                 const char *command, const char *status) {
    pthread_mutex_lock(&log_mutex);

    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);

        fprintf(f, "[%s] SOURCE=%-8s QUERY=\"%s\" COMMAND=\"%s\" STATUS=%s\n",
                ts,
                source_name(source),
                query   ? query   : "",
                command ? command : "",
                status  ? status  : "");
        fclose(f);
    }

    pthread_mutex_unlock(&log_mutex);
}
