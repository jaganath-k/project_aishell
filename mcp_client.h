#pragma once

#define MCP_HOST        "127.0.0.1"
#define MCP_PORT        9000
#define MCP_TIMEOUT_SEC 10      /* max seconds to wait for server reply  */
#define MCP_PROBE_SEC    2      /* max seconds for is-server-running probe */

typedef struct {
    int  success;            /* 1 = ok, 0 = error/timeout                */
    char result[4096];       /* output text returned by the MCP server   */
    char command_used[512];  /* shell command the server actually ran     */
    char error_msg[256];     /* human-readable error when success == 0   */
} McpResponse;

/* Return 1 if an MCP server is accepting connections on MCP_HOST:MCP_PORT,
 * 0 otherwise.  Completes in at most MCP_PROBE_SEC seconds.             */
int         mcp_is_server_running(void);

/* Send a natural-language query to the MCP server, return the response.
 * Never blocks longer than MCP_TIMEOUT_SEC seconds.                     */
McpResponse mcp_query(const char *query);
