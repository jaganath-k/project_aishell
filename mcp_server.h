#pragma once

#include <netinet/in.h>

#define MCP_SERVER_PORT   9000
#define MCP_BACKLOG       10
#define MCP_RECV_TIMEOUT  30    /* seconds before idle client is dropped */
#define MCP_MAX_CLIENTS   16

typedef struct {
    int                client_fd;
    struct sockaddr_in client_addr;
    int                client_id;
} ClientContext;

/* Start the server in a background detached pthread.
 * Returns 0 on success, -1 on bind/listen error. */
int  mcp_server_start(void);

/* Signal the server to stop and close the listening socket. */
void mcp_server_stop(void);

/* Returns 1 if the server loop is running, 0 otherwise. */
int  mcp_server_is_running(void);
