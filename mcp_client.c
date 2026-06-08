#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "mcp_client.h"
#include "aishell_log.h"

/* ── internal helpers ─────────────────────────────────────────────────────── */

/* Create and connect a non-blocking TCP socket to MCP_HOST:MCP_PORT.
 * timeout_sec: how long to wait for the connection to succeed.
 * Returns the connected fd on success, -1 on failure.                       */
static int tcp_connect(int timeout_sec) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    /* Set non-blocking so connect() returns immediately with EINPROGRESS */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(MCP_PORT);
    addr.sin_addr.s_addr = inet_addr(MCP_HOST);

    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc == 0) {
        /* Connected immediately (unusual but valid for loopback) */
        return fd;
    }
    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    /* Wait for connection via select() */
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(fd, &wset);
    struct timeval tv = { .tv_sec = timeout_sec, .tv_usec = 0 };

    int sel = select(fd + 1, NULL, &wset, NULL, &tv);
    if (sel <= 0) {
        close(fd);
        return -1;   /* timeout or select error */
    }

    /* Check if the connection actually succeeded */
    int err = 0;
    socklen_t errlen = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0 || err != 0) {
        close(fd);
        return -1;
    }

    /* Restore blocking mode for send/recv */
    fcntl(fd, F_SETFL, flags);
    return fd;
}

/* Send all bytes in buf (handles partial sends). Returns 0 on success, -1 on error. */
static int send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* Receive until the connection closes or buf is full.
 * Uses select() to enforce a timeout. Returns bytes received, -1 on error/timeout. */
static ssize_t recv_all(int fd, char *buf, size_t bufsize, int timeout_sec) {
    size_t total = 0;
    buf[0] = '\0';

    while (total < bufsize - 1) {
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(fd, &rset);
        struct timeval tv = { .tv_sec = timeout_sec, .tv_usec = 0 };

        int sel = select(fd + 1, &rset, NULL, NULL, &tv);
        if (sel < 0)  return -1;
        if (sel == 0) break;   /* timeout — return what we have */

        ssize_t n = recv(fd, buf + total, bufsize - 1 - total, 0);
        if (n < 0)  return -1;
        if (n == 0) break;     /* server closed the connection */
        total += (size_t)n;
    }
    buf[total] = '\0';
    return (ssize_t)total;
}

/* ── JSON helpers ─────────────────────────────────────────────────────────── */

/* Escape a string for embedding in a JSON value (writes into dst). */
static void json_escape(const char *src, char *dst, size_t dstsz) {
    size_t di = 0;
    for (const char *p = src; *p && di + 2 < dstsz; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"')       { dst[di++] = '\\'; dst[di++] = '"'; }
        else if (c == '\\') { dst[di++] = '\\'; dst[di++] = '\\'; }
        else if (c == '\n') { dst[di++] = '\\'; dst[di++] = 'n'; }
        else if (c == '\r') { dst[di++] = '\\'; dst[di++] = 'r'; }
        else if (c == '\t') { dst[di++] = '\\'; dst[di++] = 't'; }
        else if (c < 0x20)  { /* skip other control chars */ }
        else                  { dst[di++] = (char)c; }
    }
    dst[di] = '\0';
}

/* Extract the value of a JSON string field named key from raw JSON text.
 * Writes into out (max outsz bytes). Returns 1 if found, 0 otherwise.
 * Simple strstr-based — sufficient for well-formed single-level responses. */
static int json_extract(const char *json, const char *key,
                         char *out, size_t outsz) {
    /* Build  "key":"  pattern */
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *pos = strstr(json, pat);
    if (!pos) return 0;
    const char *val = pos + strlen(pat);

    size_t i = 0;
    while (*val && i < outsz - 1) {
        if (*val == '\\' && *(val + 1)) {
            val++;
            switch (*val) {
            case '"':  out[i++] = '"';  break;
            case '\\': out[i++] = '\\'; break;
            case 'n':  out[i++] = '\n'; break;
            case 'r':  out[i++] = '\r'; break;
            case 't':  out[i++] = '\t'; break;
            default:   out[i++] = *val; break;
            }
        } else if (*val == '"') {
            break;  /* end of string value */
        } else {
            out[i++] = *val;
        }
        val++;
    }
    out[i] = '\0';
    return 1;
}

/* ── public API ───────────────────────────────────────────────────────────── */

int mcp_is_server_running(void) {
    int fd = tcp_connect(MCP_PROBE_SEC);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

McpResponse mcp_query(const char *query) {
    McpResponse resp;
    memset(&resp, 0, sizeof(resp));

    if (!query || query[0] == '\0') {
        snprintf(resp.error_msg, sizeof(resp.error_msg), "empty query");
        aishell_log(LOG_MCP, "", "", "error-empty-query");
        return resp;
    }

    /* ── Build request JSON ── */
    char safe_query[1024];
    json_escape(query, safe_query, sizeof(safe_query));

    char request[2048];
    snprintf(request, sizeof(request),
        "{"
          "\"type\":\"mcp\","
          "\"tool\":\"run_command\","
          "\"params\":{"
            "\"query\":\"%s\","
            "\"shell\":\"aishell\","
            "\"version\":\"week8\""
          "}"
        "}\n",
        safe_query);

    /* ── Connect ── */
    int fd = tcp_connect(MCP_TIMEOUT_SEC);
    if (fd < 0) {
        snprintf(resp.error_msg, sizeof(resp.error_msg),
                 "cannot connect to MCP server at %s:%d", MCP_HOST, MCP_PORT);
        aishell_log(LOG_MCP, query, "", "error-connect");
        return resp;
    }

    /* ── Send ── */
    if (send_all(fd, request, strlen(request)) < 0) {
        snprintf(resp.error_msg, sizeof(resp.error_msg), "send failed: %s", strerror(errno));
        close(fd);
        aishell_log(LOG_MCP, query, "", "error-send");
        return resp;
    }

    /* ── Receive ── */
    char raw[8192];
    ssize_t n = recv_all(fd, raw, sizeof(raw), MCP_TIMEOUT_SEC);
    close(fd);

    if (n < 0) {
        snprintf(resp.error_msg, sizeof(resp.error_msg), "recv failed: %s", strerror(errno));
        aishell_log(LOG_MCP, query, "", "error-recv");
        return resp;
    }
    if (n == 0) {
        snprintf(resp.error_msg, sizeof(resp.error_msg),
                 "MCP server closed connection without response");
        aishell_log(LOG_MCP, query, "", "error-no-response");
        return resp;
    }

    /* ── Parse response ── */
    char status_val[32] = "";
    json_extract(raw, "status", status_val, sizeof(status_val));

    if (strcmp(status_val, "ok") == 0) {
        resp.success = 1;
        json_extract(raw, "result",       resp.result,       sizeof(resp.result));
        json_extract(raw, "command_used", resp.command_used, sizeof(resp.command_used));
        aishell_log(LOG_MCP, query, resp.command_used, "ok");
    } else {
        resp.success = 0;
        char msg[256] = "";
        json_extract(raw, "message", msg, sizeof(msg));
        if (msg[0] == '\0')
            snprintf(msg, sizeof(msg), "server returned status: %s",
                     status_val[0] ? status_val : "unknown");
        snprintf(resp.error_msg, sizeof(resp.error_msg), "%s", msg);
        aishell_log(LOG_MCP, query, "", status_val[0] ? status_val : "error");
    }

    return resp;
}
