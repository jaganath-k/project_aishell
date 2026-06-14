/* cmd_server.c — "server" built-in command for AiShell Week 9.
 * Subcommands: status | start | stop | config [key=value]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "cmd_spec.h"
#include "cmd_help_json.h"
#include "mcp_server.h"
#include "config.h"

static const char *CONFIG_PATH = "aishell.conf";

static void server_print_usage(FILE *out);

static int server_run(int argc, char **argv) {
    if (argc < 2) {
        server_print_usage(stdout);
        return 1;
    }

    const char *sub = argv[1];

    if (!strcmp(sub, "status")) {
        int running = mcp_server_is_running();
        printf("MCP/FTP server: %s\n", running ? "running" : "stopped");
        if (running)
            printf("  port           : %d\n", g_config.server_port);
        return 0;
    }

    if (!strcmp(sub, "start")) {
        if (mcp_server_is_running()) {
            printf("server: already running on port %d\n", g_config.server_port);
            return 0;
        }
        int rc = mcp_server_start();
        if (rc == 0)
            printf("server: started on port %d\n", g_config.server_port);
        else
            fprintf(stderr, "server: failed to start\n");
        return rc;
    }

    if (!strcmp(sub, "stop")) {
        if (!mcp_server_is_running()) {
            printf("server: not running\n");
            return 0;
        }
        mcp_server_stop();
        printf("server: stopped\n");
        return 0;
    }

    if (!strcmp(sub, "config")) {
        if (argc >= 3) {
            /* server config key=value */
            return config_set(CONFIG_PATH, argv[2]) == 0 ? 0 : 1;
        }
        config_print();
        return 0;
    }

    if (!strcmp(sub, "--help") || !strcmp(sub, "-h")) {
        server_print_usage(stdout);
        return 0;
    }

    fprintf(stderr, "server: unknown subcommand '%s'\n", sub);
    server_print_usage(stderr);
    return 1;
}

static void server_print_usage(FILE *out) {
    fprintf(out,
        "Usage: server <subcommand> [args]\n"
        "\n"
        "Subcommands:\n"
        "  status            Print server running/stopped, port\n"
        "  start             Start the MCP/FTP server\n"
        "  stop              Stop the MCP/FTP server\n"
        "  config            Print all configuration settings\n"
        "  config key=value  Update one config setting and save\n"
        "\n"
        "Examples:\n"
        "  server status\n"
        "  server config server_timeout=60\n"
        "  server config server_enabled=0\n");
}

static cmd_spec_t cmd_server_spec = {
    .name       = "server",
    .summary    = "manage the MCP/FTP server",
    .long_help  = "Control the built-in TCP server (port 9000) that handles\n"
                  "FTP-style file transfers and MCP JSON tool calls.\n"
                  "Use 'server config key=value' to update aishell.conf at runtime.",
    .run         = server_run,
    .print_usage = server_print_usage,
};

void register_server_command(void) {
    register_command(&cmd_server_spec);
}
