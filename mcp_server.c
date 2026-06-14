/* mcp_server.c — MCP Server: TCP listener on port 9000, pthread per client.
 * Handles two client types detected from the first byte:
 *   '{' → MCP JSON tool call (run_command / list_tools / get_status / get_registry)
 *   other / timeout → FTP protocol (USER/QUIT/PORT/STOR/RETR/LIST/MKD)
 * Week 9 — new server-side complement to Week 8's mcp_client.c.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

#include "mcp_server.h"
#include "ftp_handler.h"
#include "cmd_spec.h"
#include "aishell_log.h"

/* ── Server state ──────────────────────────────────────────────────── */
static volatile sig_atomic_t server_running    = 0;
static int                   server_fd         = -1;
static time_t                server_start_time = 0;
static pthread_mutex_t       metrics_mtx       = PTHREAD_MUTEX_INITIALIZER;
static int                   g_connected       = 0;
static long                  g_total_requests  = 0;
static int                   g_next_client_id  = 0;

/* ── Command allowlist (safe, read-only built-ins) ─────────────────── */
static const char * const ALLOWLIST[] = {
    "ls","cat","grep","echo","find","du","df","ps",
    "wc","sort","uniq","cut","tr","head","tail",
    "pwd","date","which","stat","file",
    NULL
};

static int is_allowed_command(const char *cmd) {
    if (!cmd || !*cmd) return 0;
    for (int i = 0; ALLOWLIST[i]; i++)
        if (strcmp(cmd, ALLOWLIST[i]) == 0) return 1;
    return 0;
}

/* ── Read one line from fd, stripping \r\n ─────────────────────────── */
static ssize_t read_line(int fd, char *buf, size_t len) {
    size_t i = 0;
    while (i < len - 1) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) break;
        if (c == '\n') break;
        if (c != '\r') buf[i++] = c;
    }
    buf[i] = '\0';
    return (ssize_t)i;
}

/* ── Send length-prefixed frame: "<len>\n<body>\n" ─────────────────── */
static void send_framed(int fd, const char *body) {
    char hdr[32];
    size_t blen = strlen(body);
    int hlen = snprintf(hdr, sizeof(hdr), "%zu\n", blen);
    send(fd, hdr,  (size_t)hlen, MSG_NOSIGNAL);
    send(fd, body, blen,         MSG_NOSIGNAL);
    send(fd, "\n", 1,            MSG_NOSIGNAL);
}

/* ── Minimal JSON string-value extractor ───────────────────────────── */
static int json_str(const char *json, const char *key,
                    char *out, size_t outlen) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i < outlen - 1)
            out[i++] = *p++;
        out[i] = '\0';
        return 1;
    }
    /* non-string value */
    size_t i = 0;
    while (*p && *p != ',' && *p != '}' && *p != '\n' && i < outlen - 1)
        out[i++] = *p++;
    out[i] = '\0';
    return 1;
}

/* ── JSON-escape src into dst (returns chars written) ─────────────── */
static int json_escape(const char *src, char *dst, size_t dstsz) {
    size_t i = 0;
    for (; *src && i + 4 < dstsz; src++) {
        unsigned char c = (unsigned char)*src;
        if      (c == '"')  { dst[i++] = '\\'; dst[i++] = '"'; }
        else if (c == '\\') { dst[i++] = '\\'; dst[i++] = '\\'; }
        else if (c == '\n') { dst[i++] = '\\'; dst[i++] = 'n'; }
        else if (c == '\r') { dst[i++] = '\\'; dst[i++] = 'r'; }
        else if (c == '\t') { dst[i++] = '\\'; dst[i++] = 't'; }
        else if (c >= 0x20) dst[i++] = c;
    }
    dst[i] = '\0';
    return (int)i;
}

/* ── Reject obvious shell injection in args ────────────────────────── */
static int is_safe_args(const char *s) {
    static const char * const patterns[] =
        { ";", "&&", "||", "`", "$(", ">&", NULL };
    for (int i = 0; patterns[i]; i++)
        if (strstr(s, patterns[i])) return 0;
    return 1;
}

/* ══════════════════ MCP Tool Handlers ═══════════════════════════════ */

static void tool_run_command(int fd, const char *json) {
    char cmd_name[128] = {0};
    char cmd_args[512] = {0};

    if (!json_str(json, "command", cmd_name, sizeof(cmd_name))) {
        send_framed(fd, "{\"status\":\"error\","
                        "\"message\":\"missing 'command' field\"}");
        return;
    }
    json_str(json, "args", cmd_args, sizeof(cmd_args));

    if (!is_allowed_command(cmd_name)) {
        send_framed(fd, "{\"status\":\"error\","
                        "\"message\":\"command not in allowlist\"}");
        aishell_log(LOG_MCP, cmd_name, "blocked", "not in allowlist");
        return;
    }
    if (cmd_args[0] && !is_safe_args(cmd_args)) {
        send_framed(fd, "{\"status\":\"error\","
                        "\"message\":\"invalid characters in args\"}");
        return;
    }
    /* truncate oversized args */
    if (strlen(cmd_args) > 512) cmd_args[512] = '\0';

    char cmd_line[1024];
    if (cmd_args[0])
        snprintf(cmd_line, sizeof(cmd_line), "%s %s 2>&1", cmd_name, cmd_args);
    else
        snprintf(cmd_line, sizeof(cmd_line), "%s 2>&1", cmd_name);

    FILE *fp = popen(cmd_line, "r");
    if (!fp) {
        send_framed(fd, "{\"status\":\"error\",\"message\":\"popen failed\"}");
        return;
    }
    char raw[4096] = {0};
    size_t nr = fread(raw, 1, sizeof(raw) - 1, fp);
    raw[nr] = '\0';
    pclose(fp);

    char esc[8192] = {0};
    json_escape(raw, esc, sizeof(esc));

    char resp[8448];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"tool\":\"run_command\","
             "\"result\":\"%s\"}", esc);
    send_framed(fd, resp);
    aishell_log(LOG_MCP, cmd_name, cmd_line, "executed");
}

static void tool_list_tools(int fd) {
    static const char RESP[] =
        "{\"status\":\"ok\",\"tool\":\"list_tools\",\"result\":["
          "{\"name\":\"run_command\","
           "\"description\":\"Run an allowlisted AiShell built-in command\","
           "\"parameters\":{\"type\":\"object\","
             "\"properties\":{"
               "\"command\":{\"type\":\"string\",\"description\":\"Command name\"},"
               "\"args\":{\"type\":\"string\",\"description\":\"Arguments\"}"
             "},\"required\":[\"command\"]},"
           "\"output_format\":\"text\"},"
          "{\"name\":\"list_tools\","
           "\"description\":\"List all available MCP tools\","
           "\"parameters\":{\"type\":\"object\",\"properties\":{}},"
           "\"output_format\":\"json\"},"
          "{\"name\":\"get_status\","
           "\"description\":\"Get AiShell server status and metrics\","
           "\"parameters\":{\"type\":\"object\",\"properties\":{}},"
           "\"output_format\":\"json\"},"
          "{\"name\":\"get_registry\","
           "\"description\":\"Get all registered commands with descriptions\","
           "\"parameters\":{\"type\":\"object\",\"properties\":{}},"
           "\"output_format\":\"json\"}"
        "]}";
    send_framed(fd, RESP);
}

static void tool_get_status(int fd) {
    time_t now    = time(NULL);
    long   uptime = (long)(now - server_start_time);

    pthread_mutex_lock(&metrics_mtx);
    int  cli  = g_connected;
    long reqs = g_total_requests;
    pthread_mutex_unlock(&metrics_mtx);

    char resp[512];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"tool\":\"get_status\",\"result\":{"
             "\"server_port\":%d,"
             "\"uptime_seconds\":%ld,"
             "\"connected_clients\":%d,"
             "\"total_requests\":%ld,"
             "\"log_file\":\"aishell_calls.log\"}}",
             MCP_SERVER_PORT, uptime, cli, reqs);
    send_framed(fd, resp);
}

/* Callback state for iterating registered commands */
typedef struct { char *buf; size_t len; size_t cap; int first; } RegBuf;

static void rb_append(RegBuf *rb, const char *s) {
    size_t n = strlen(s);
    if (rb->len + n + 1 < rb->cap) {
        memcpy(rb->buf + rb->len, s, n);
        rb->len += n;
        rb->buf[rb->len] = '\0';
    }
}

static void reg_cmd_cb(const cmd_spec_t *spec, void *ud) {
    RegBuf *rb = ud;
    if (!rb->first) rb_append(rb, ",");
    rb->first = 0;
    char ename[128], esumm[256];
    json_escape(spec->name,                      ename,  sizeof(ename));
    json_escape(spec->summary ? spec->summary : "", esumm, sizeof(esumm));
    char entry[512];
    snprintf(entry, sizeof(entry),
             "{\"name\":\"%s\",\"summary\":\"%s\"}", ename, esumm);
    rb_append(rb, entry);
}

static void tool_get_registry(int fd) {
    char *buf = malloc(32768);
    if (!buf) {
        send_framed(fd, "{\"status\":\"error\",\"message\":\"out of memory\"}");
        return;
    }
    RegBuf rb = { buf, 0, 32768, 1 };
    rb_append(&rb, "{\"status\":\"ok\",\"tool\":\"get_registry\",\"commands\":[");
    for_each_command(reg_cmd_cb, &rb);
    rb_append(&rb, "]}");
    send_framed(fd, buf);
    free(buf);
}

/* ── Security: verify peer is localhost ────────────────────────────── */
static int peer_is_localhost(int fd) {
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    if (getpeername(fd, (struct sockaddr *)&peer, &plen) < 0) return 0;
    return peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK);
}

/* ── MCP JSON dispatcher ────────────────────────────────────────────── */
static void handle_mcp_json(int fd, const char *first_line, int client_id) {
    (void)client_id;

    if (!peer_is_localhost(fd)) {
        send_framed(fd, "{\"status\":\"error\","
                        "\"message\":\"only localhost connections allowed\"}");
        aishell_log(LOG_MCP, "server", "non-local MCP client", "rejected");
        return;
    }

    /* Guard against empty / malformed JSON */
    if (!first_line || first_line[0] != '{') {
        send_framed(fd, "{\"status\":\"error\",\"message\":\"invalid JSON\"}");
        return;
    }

    char tool[64] = {0};
    if (!json_str(first_line, "tool", tool, sizeof(tool))) {
        send_framed(fd, "{\"status\":\"error\","
                        "\"message\":\"missing 'tool' field\"}");
        return;
    }

    pthread_mutex_lock(&metrics_mtx);
    g_total_requests++;
    pthread_mutex_unlock(&metrics_mtx);

    if      (strcmp(tool, "run_command")  == 0) tool_run_command(fd, first_line);
    else if (strcmp(tool, "list_tools")   == 0) tool_list_tools(fd);
    else if (strcmp(tool, "get_status")   == 0) tool_get_status(fd);
    else if (strcmp(tool, "get_registry") == 0) tool_get_registry(fd);
    else {
        char etool[64], err[128];
        json_escape(tool, etool, sizeof(etool));
        snprintf(err, sizeof(err),
                 "{\"status\":\"error\",\"message\":\"unknown tool '%s'\"}",
                 etool);
        send_framed(fd, err);
    }

    aishell_log(LOG_MCP, tool, "mcp-json", "handled");
}

/* ══════════════════ Client Thread ═══════════════════════════════════ */

static void *client_handler(void *arg) {
    ClientContext *ctx = arg;
    int fd  = ctx->client_fd;
    int cid = ctx->client_id;

    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ctx->client_addr.sin_addr, ip, sizeof(ip));
    fprintf(stderr, "[mcp-server] client %d connected from %s:%d\n",
            cid, ip, ntohs(ctx->client_addr.sin_port));

    /* Short peek timeout: FTP is server-speaks-first; MCP clients send first.
     * If nothing arrives in 300 ms, treat as FTP. */
    {
        struct timeval tv = { .tv_sec = 0, .tv_usec = 300000 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    char peek[2] = {0};
    ssize_t pn = recv(fd, peek, 1, MSG_PEEK);

    /* Restore main idle timeout */
    {
        struct timeval tv = { .tv_sec = MCP_RECV_TIMEOUT, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    if (pn > 0 && peek[0] == '{') {
        /* MCP JSON client — consume + parse the first line */
        char line[4097] = {0};
        read_line(fd, line, sizeof(line));
        handle_mcp_json(fd, line, cid);
    } else {
        /* FTP client (server-speaks-first) or timed-out peek */
        char first_line[1025] = {0};
        if (pn > 0) read_line(fd, first_line, sizeof(first_line));
        handle_ftp_session(fd, first_line);
    }

    close(fd);

    pthread_mutex_lock(&metrics_mtx);
    g_connected--;
    pthread_mutex_unlock(&metrics_mtx);

    fprintf(stderr, "[mcp-server] client %d disconnected\n", cid);
    free(ctx);
    return NULL;
}

/* ══════════════════ Server Accept Loop ══════════════════════════════ */

static void *server_loop(void *arg) {
    (void)arg;

    while (server_running) {
        struct sockaddr_in cli_addr;
        socklen_t          cli_len = sizeof(cli_addr);
        int cfd = accept(server_fd, (struct sockaddr *)&cli_addr, &cli_len);

        if (cfd < 0) {
            if (errno == EINTR)     continue;  /* signal interrupted */
            if (!server_running)   break;      /* stop() closed the fd */
            perror("[mcp-server] accept");
            continue;
        }

        pthread_mutex_lock(&metrics_mtx);
        int cid = ++g_next_client_id;
        g_connected++;
        pthread_mutex_unlock(&metrics_mtx);

        ClientContext *ctx = malloc(sizeof(*ctx));
        if (!ctx) {
            close(cfd);
            pthread_mutex_lock(&metrics_mtx);
            g_connected--;
            pthread_mutex_unlock(&metrics_mtx);
            continue;
        }
        ctx->client_fd   = cfd;
        ctx->client_addr = cli_addr;
        ctx->client_id   = cid;

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_handler, ctx) != 0) {
            perror("[mcp-server] pthread_create");
            close(cfd);
            free(ctx);
            pthread_mutex_lock(&metrics_mtx);
            g_connected--;
            pthread_mutex_unlock(&metrics_mtx);
        } else {
            pthread_detach(tid);
        }
    }

    fprintf(stderr, "[mcp-server] server loop exited\n");
    return NULL;
}

/* ══════════════════ Public API ══════════════════════════════════════ */

int mcp_server_start(void) {
    if (server_running) return 0;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("[mcp-server] socket"); return -1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(MCP_SERVER_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[mcp-server] bind");
        close(server_fd); server_fd = -1; return -1;
    }
    if (listen(server_fd, MCP_BACKLOG) < 0) {
        perror("[mcp-server] listen");
        close(server_fd); server_fd = -1; return -1;
    }

    server_running    = 1;
    server_start_time = time(NULL);

    pthread_t tid;
    if (pthread_create(&tid, NULL, server_loop, NULL) != 0) {
        perror("[mcp-server] pthread_create");
        server_running = 0;
        close(server_fd); server_fd = -1; return -1;
    }
    pthread_detach(tid);

    fprintf(stderr, "[mcp-server] Listening on port %d\n", MCP_SERVER_PORT);
    return 0;
}

void mcp_server_stop(void) {
    if (!server_running) return;
    server_running = 0;
    if (server_fd >= 0) {
        /* Close the listening socket so accept() unblocks and exits. */
        int fd = server_fd;
        server_fd = -1;
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    fprintf(stderr, "[mcp-server] stop requested\n");
}

int mcp_server_is_running(void) {
    return server_running;
}
