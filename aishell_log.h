#pragma once

typedef enum {
    LOG_REGISTRY,   /* command matched from commands.json      */
    LOG_MCP,        /* result from MCP server on localhost:9000 */
    LOG_AI,         /* suggestion from OpenRouter AI           */
    LOG_EXEC        /* direct shell command executed            */
} LogSource;

/* Append one line to aishell_calls.log (thread-safe).
 *
 * Format (tab-separated):
 *   [YYYY-MM-DD HH:MM:SS] SOURCE=<src> QUERY="<q>" COMMAND="<cmd>" STATUS=<s>
 */
void aishell_log(LogSource source, const char *query,
                 const char *command, const char *status);
