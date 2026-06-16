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
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <regex.h>

#include "mcp_server.h"
#include "ftp_handler.h"
#include "cmd_spec.h"
#include "aishell_log.h"
#include "jobs_api.h"
#include "cmd_edit.h"

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
    /* Week 9 tools + Week 10 fs.* tools */
    static const char RESP[] =
        "{\"status\":\"ok\",\"tool\":\"list_tools\",\"result\":["
          /* ── Week 9 ── */
          "{\"name\":\"run_command\","
           "\"description\":\"Run an allowlisted AiShell built-in command\","
           "\"parameters\":{\"type\":\"object\","
             "\"properties\":{"
               "\"command\":{\"type\":\"string\"},"
               "\"args\":{\"type\":\"string\"}"
             "},\"required\":[\"command\"]}},"
          "{\"name\":\"list_tools\","
           "\"description\":\"List all available MCP tools\","
           "\"parameters\":{\"type\":\"object\",\"properties\":{}}},"
          "{\"name\":\"get_status\","
           "\"description\":\"Get server status and metrics\","
           "\"parameters\":{\"type\":\"object\",\"properties\":{}}},"
          "{\"name\":\"get_registry\","
           "\"description\":\"Get all registered shell commands\","
           "\"parameters\":{\"type\":\"object\",\"properties\":{}}},"
          /* ── Week 10: fs.* ── */
          "{\"name\":\"fs.list\","
           "\"description\":\"List directory contents\","
           "\"parameters\":{\"type\":\"object\","
             "\"properties\":{\"path\":{\"type\":\"string\"}},"
             "\"required\":[\"path\"]}},"
          "{\"name\":\"fs.read\","
           "\"description\":\"Read file content (up to 64 KB)\","
           "\"parameters\":{\"type\":\"object\","
             "\"properties\":{"
               "\"path\":{\"type\":\"string\"},"
               "\"offset\":{\"type\":\"integer\"},"
               "\"length\":{\"type\":\"integer\"}"
             "},\"required\":[\"path\"]}},"
          "{\"name\":\"fs.write\","
           "\"description\":\"Write content to a file\","
           "\"parameters\":{\"type\":\"object\","
             "\"properties\":{"
               "\"path\":{\"type\":\"string\"},"
               "\"content\":{\"type\":\"string\"},"
               "\"mode\":{\"type\":\"string\",\"enum\":[\"overwrite\",\"create_only\"]}"
             "},\"required\":[\"path\",\"content\"]}},"
          "{\"name\":\"fs.append\","
           "\"description\":\"Append content to a file\","
           "\"parameters\":{\"type\":\"object\","
             "\"properties\":{"
               "\"path\":{\"type\":\"string\"},"
               "\"content\":{\"type\":\"string\"}"
             "},\"required\":[\"path\",\"content\"]}},"
          "{\"name\":\"fs.stat\","
           "\"description\":\"Get file or directory metadata\","
           "\"parameters\":{\"type\":\"object\","
             "\"properties\":{\"path\":{\"type\":\"string\"}},"
             "\"required\":[\"path\"]}},"
          "{\"name\":\"fs.search\","
           "\"description\":\"Recursive regex search in files (max 100 matches)\","
           "\"parameters\":{\"type\":\"object\","
             "\"properties\":{"
               "\"path\":{\"type\":\"string\"},"
               "\"pattern\":{\"type\":\"string\"}"
             "},\"required\":[\"path\",\"pattern\"]}},"
          /* ── Week 10: proc.* process management tools ── */
          "{\"name\":\"proc.list\","
           "\"description\":\"List all shell background jobs (process and thread)\","
           "\"parameters\":{\"type\":\"object\",\"properties\":{}}},"
          "{\"name\":\"proc.kill\","
           "\"description\":\"Send a signal to a background job by job_id\","
           "\"parameters\":{\"type\":\"object\","
             "\"properties\":{"
               "\"job_id\":{\"type\":\"integer\"},"
               "\"signal\":{\"type\":\"string\","
                 "\"description\":\"Signal name (SIGTERM, SIGKILL, SIGINT, SIGHUP, SIGSTOP, SIGCONT) or number\"}"
             "},\"required\":[\"job_id\"]}},"
          "{\"name\":\"proc.wait\","
           "\"description\":\"Block until a background process job exits; returns exit_code\","
           "\"parameters\":{\"type\":\"object\","
             "\"properties\":{"
               "\"job_id\":{\"type\":\"integer\"}"
             "},\"required\":[\"job_id\"]}},"
          /* ── Week 10: env.* environment tools ── */
          "{\"name\":\"env.get\","
           "\"description\":\"Get the value of an environment variable (null if unset)\","
           "\"parameters\":{\"type\":\"object\","
             "\"properties\":{\"name\":{\"type\":\"string\"}},"
             "\"required\":[\"name\"]}},"
          "{\"name\":\"env.set\","
           "\"description\":\"Set an environment variable (name must match [A-Za-z_][A-Za-z0-9_]*)\","
           "\"parameters\":{\"type\":\"object\","
             "\"properties\":{"
               "\"name\":{\"type\":\"string\"},"
               "\"value\":{\"type\":\"string\"}"
             "},\"required\":[\"name\",\"value\"]}},"
          "{\"name\":\"env.list\","
           "\"description\":\"List all environment variables as a JSON object\","
           "\"parameters\":{\"type\":\"object\",\"properties\":{}}},"
          /* ── Week 10: edit.* text editing tools ── */
          "{\"name\":\"edit.replace_line\","
           "\"description\":\"Replace line N (1-indexed) in a file with new text\","
           "\"parameters\":{\"type\":\"object\","
             "\"properties\":{"
               "\"path\":{\"type\":\"string\"},"
               "\"line\":{\"type\":\"integer\"},"
               "\"text\":{\"type\":\"string\"}"
             "},\"required\":[\"path\",\"line\",\"text\"]}},"
          "{\"name\":\"edit.insert_line\","
           "\"description\":\"Insert text before line N (N=count+1 appends)\","
           "\"parameters\":{\"type\":\"object\","
             "\"properties\":{"
               "\"path\":{\"type\":\"string\"},"
               "\"line\":{\"type\":\"integer\"},"
               "\"text\":{\"type\":\"string\"}"
             "},\"required\":[\"path\",\"line\",\"text\"]}},"
          "{\"name\":\"edit.delete_line\","
           "\"description\":\"Delete line N from a file\","
           "\"parameters\":{\"type\":\"object\","
             "\"properties\":{"
               "\"path\":{\"type\":\"string\"},"
               "\"line\":{\"type\":\"integer\"}"
             "},\"required\":[\"path\",\"line\"]}},"
          "{\"name\":\"edit.replace\","
           "\"description\":\"Global literal-string replace in a file\","
           "\"parameters\":{\"type\":\"object\","
             "\"properties\":{"
               "\"path\":{\"type\":\"string\"},"
               "\"pattern\":{\"type\":\"string\"},"
               "\"replacement\":{\"type\":\"string\"}"
             "},\"required\":[\"path\",\"pattern\",\"replacement\"]}}"
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

/* ══════════════════ Week 10: fs.* MCP Tools ═══════════════════════ */

/* Reject paths containing ".." (directory traversal). */
static int path_is_safe(const char *p) {
    return p && *p && !strstr(p, "..");
}

/* Convenience: send a JSON error with tool name embedded. */
static void send_error(int fd, const char *tool, const char *msg) {
    char emsg[256], buf[512];
    json_escape(msg, emsg, sizeof(emsg));
    snprintf(buf, sizeof(buf),
             "{\"status\":\"error\",\"tool\":\"%s\",\"message\":\"%s\"}",
             tool, emsg);
    send_framed(fd, buf);
}

/* Unescape JSON string escapes (\n \t \r \\ \") in-place into dst. */
static void json_unescape(const char *src, char *dst, size_t dstsz) {
    size_t i = 0;
    for (; *src && i + 1 < dstsz; src++) {
        if (*src == '\\' && *(src + 1)) {
            src++;
            switch (*src) {
            case 'n':  dst[i++] = '\n'; break;
            case 't':  dst[i++] = '\t'; break;
            case 'r':  dst[i++] = '\r'; break;
            case '"':  dst[i++] = '"';  break;
            case '\\': dst[i++] = '\\'; break;
            default:   dst[i++] = '\\'; if (i + 1 < dstsz) dst[i++] = *src; break;
            }
        } else {
            dst[i++] = *src;
        }
    }
    dst[i] = '\0';
}

/* Return 1 if file likely contains binary data (NUL byte in first 512). */
static int is_binary_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 1;
    char buf[512];
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    for (ssize_t i = 0; i < n; i++)
        if (buf[i] == '\0') return 1;
    return 0;
}

/* ── fs.list ─────────────────────────────────────────────────────────*/
static void tool_fs_list(int fd, const char *json) {
    char path[512] = ".";
    json_str(json, "path", path, sizeof(path));

    if (!path_is_safe(path)) {
        send_error(fd, "fs.list", "path traversal rejected"); return;
    }
    DIR *d = opendir(path);
    if (!d) {
        char m[128]; snprintf(m, sizeof(m), "cannot open: %s", strerror(errno));
        send_error(fd, "fs.list", m); return;
    }

    char *buf = malloc(65536);
    if (!buf) { closedir(d); send_error(fd, "fs.list", "out of memory"); return; }

    int off = snprintf(buf, 65536,
                       "{\"status\":\"ok\",\"tool\":\"fs.list\",\"result\":[");
    int first = 1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' && (ent->d_name[1] == '\0' ||
            (ent->d_name[1] == '.' && ent->d_name[2] == '\0'))) continue;

        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);

        struct stat st;
        const char *type = "unknown";
        long  size  = 0;
        char  mode[8]  = "0000";
        char  mtime[32] = "";
        if (lstat(full, &st) == 0) {
            if      (S_ISDIR(st.st_mode)) type = "dir";
            else if (S_ISLNK(st.st_mode)) type = "link";
            else                           type = "file";
            size = (long)st.st_size;
            snprintf(mode, sizeof(mode), "%04o", (unsigned)(st.st_mode & 0777));
            struct tm *tm = gmtime(&st.st_mtime);
            if (tm) strftime(mtime, sizeof(mtime), "%Y-%m-%dT%H:%M:%SZ", tm);
        }
        char ename[256];
        json_escape(ent->d_name, ename, sizeof(ename));
        char entry[512];
        int elen = snprintf(entry, sizeof(entry),
                            "%s{\"name\":\"%s\",\"type\":\"%s\","
                            "\"size\":%ld,\"mode\":\"%s\",\"mtime\":\"%s\"}",
                            first ? "" : ",", ename, type, size, mode, mtime);
        if (off + elen + 4 < 65536) {
            memcpy(buf + off, entry, (size_t)elen);
            off += elen;
            first = 0;
        }
    }
    closedir(d);
    off += snprintf(buf + off, 65536 - (size_t)off, "]}");
    send_framed(fd, buf);
    free(buf);
    aishell_log(LOG_MCP, "fs.list", path, "ok");
}

/* ── fs.read ─────────────────────────────────────────────────────────*/
static void tool_fs_read(int fd, const char *json) {
    char path[512] = "", offset_s[32] = "0", length_s[32] = "0";

    if (!json_str(json, "path", path, sizeof(path)) || !path[0]) {
        send_error(fd, "fs.read", "missing 'path'"); return;
    }
    json_str(json, "offset", offset_s, sizeof(offset_s));
    json_str(json, "length", length_s, sizeof(length_s));

    if (!path_is_safe(path)) {
        send_error(fd, "fs.read", "path traversal rejected"); return;
    }

    int off = atoi(offset_s);
    int len = atoi(length_s);
    if (off < 0) off = 0;
    if (len <= 0 || len > 65536) len = 65536;

    int f = open(path, O_RDONLY);
    if (f < 0) {
        char m[128]; snprintf(m, sizeof(m), "cannot open: %s", strerror(errno));
        send_error(fd, "fs.read", m); return;
    }
    if (off > 0) lseek(f, (off_t)off, SEEK_SET);

    char *raw = malloc((size_t)len + 1);
    if (!raw) { close(f); send_error(fd, "fs.read", "out of memory"); return; }
    ssize_t nr = read(f, raw, (size_t)len);
    close(f);
    if (nr < 0) nr = 0;
    raw[nr] = '\0';

    char *esc = malloc((size_t)nr * 6 + 32);
    if (!esc) { free(raw); send_error(fd, "fs.read", "out of memory"); return; }
    json_escape(raw, esc, (size_t)nr * 6 + 32);
    free(raw);

    size_t rlen = strlen(esc) + 128;
    char *resp = malloc(rlen);
    if (!resp) { free(esc); send_error(fd, "fs.read", "out of memory"); return; }
    snprintf(resp, rlen,
             "{\"status\":\"ok\",\"tool\":\"fs.read\","
             "\"bytes_read\":%zd,\"content\":\"%s\"}", nr, esc);
    send_framed(fd, resp);
    free(esc); free(resp);
    aishell_log(LOG_MCP, "fs.read", path, "ok");
}

/* ── fs.write ────────────────────────────────────────────────────────*/
static void tool_fs_write(int fd, const char *json) {
    char path[512] = "", raw_content[4096] = "", mode[32] = "overwrite";

    if (!json_str(json, "path", path, sizeof(path)) || !path[0]) {
        send_error(fd, "fs.write", "missing 'path'"); return;
    }
    json_str(json, "content", raw_content, sizeof(raw_content));
    json_str(json, "mode",    mode,        sizeof(mode));

    if (!path_is_safe(path)) {
        send_error(fd, "fs.write", "path traversal rejected"); return;
    }

    char content[4096];
    json_unescape(raw_content, content, sizeof(content));

    int flags = O_WRONLY | O_CREAT;
    flags |= (strcmp(mode, "create_only") == 0) ? O_EXCL : O_TRUNC;

    int f = open(path, flags, 0644);
    if (f < 0) {
        char m[128]; snprintf(m, sizeof(m), "open failed: %s", strerror(errno));
        send_error(fd, "fs.write", m); return;
    }
    size_t clen = strlen(content);
    ssize_t nw = write(f, content, clen);
    close(f);

    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"tool\":\"fs.write\",\"bytes_written\":%zd}", nw);
    send_framed(fd, resp);
    aishell_log(LOG_MCP, "fs.write", path, "ok");
}

/* ── fs.append ───────────────────────────────────────────────────────*/
static void tool_fs_append(int fd, const char *json) {
    char path[512] = "", raw_content[4096] = "";

    if (!json_str(json, "path", path, sizeof(path)) || !path[0]) {
        send_error(fd, "fs.append", "missing 'path'"); return;
    }
    json_str(json, "content", raw_content, sizeof(raw_content));

    if (!path_is_safe(path)) {
        send_error(fd, "fs.append", "path traversal rejected"); return;
    }

    char content[4096];
    json_unescape(raw_content, content, sizeof(content));

    int f = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (f < 0) {
        char m[128]; snprintf(m, sizeof(m), "open failed: %s", strerror(errno));
        send_error(fd, "fs.append", m); return;
    }
    size_t clen = strlen(content);
    ssize_t nw = write(f, content, clen);
    close(f);

    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"tool\":\"fs.append\",\"bytes_written\":%zd}", nw);
    send_framed(fd, resp);
    aishell_log(LOG_MCP, "fs.append", path, "ok");
}

/* ── fs.stat ─────────────────────────────────────────────────────────*/
static void tool_fs_stat(int fd, const char *json) {
    char path[512] = "";

    if (!json_str(json, "path", path, sizeof(path)) || !path[0]) {
        send_error(fd, "fs.stat", "missing 'path'"); return;
    }
    if (!path_is_safe(path)) {
        send_error(fd, "fs.stat", "path traversal rejected"); return;
    }

    struct stat st;
    if (stat(path, &st) < 0) {
        char m[128]; snprintf(m, sizeof(m), "stat failed: %s", strerror(errno));
        send_error(fd, "fs.stat", m); return;
    }

    char mode[8];
    snprintf(mode, sizeof(mode), "%04o", (unsigned)(st.st_mode & 0777));

    char resp[512];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"tool\":\"fs.stat\",\"result\":{"
             "\"size\":%ld,\"mode\":\"%s\","
             "\"is_dir\":%s,\"is_file\":%s,\"is_link\":%s,"
             "\"mtime\":%ld,\"uid\":%u,\"gid\":%u}}",
             (long)st.st_size, mode,
             S_ISDIR(st.st_mode) ? "true" : "false",
             S_ISREG(st.st_mode) ? "true" : "false",
             S_ISLNK(st.st_mode) ? "true" : "false",
             (long)st.st_mtime,
             (unsigned)st.st_uid, (unsigned)st.st_gid);
    send_framed(fd, resp);
    aishell_log(LOG_MCP, "fs.stat", path, "ok");
}

/* ── fs.search helpers ───────────────────────────────────────────────*/
#define FS_SEARCH_MAX 100

typedef struct {
    regex_t *re;
    char    *buf;
    size_t   buf_size;
    size_t   buf_len;
    int      count;
    int      first;
} FsSearchCtx;

static void fss_append(FsSearchCtx *c, const char *path,
                        int lineno, const char *line) {
    if (c->count >= FS_SEARCH_MAX) return;
    char ep[512], el[512];
    json_escape(path, ep, sizeof(ep));
    json_escape(line, el, sizeof(el));
    char entry[1280];
    int elen = snprintf(entry, sizeof(entry),
                        "%s\"%s:%d:%s\"",
                        c->first ? "" : ",", ep, lineno, el);
    if (c->buf_len + (size_t)elen + 8 < c->buf_size) {
        memcpy(c->buf + c->buf_len, entry, (size_t)elen);
        c->buf_len += (size_t)elen;
        c->buf[c->buf_len] = '\0';
        c->first = 0;
        c->count++;
    }
}

static void fss_file(FsSearchCtx *c, const char *path) {
    if (is_binary_file(path)) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024]; int lineno = 0;
    while (fgets(line, sizeof(line), f) && c->count < FS_SEARCH_MAX) {
        lineno++;
        size_t n = strlen(line);
        if (n > 0 && line[n - 1] == '\n') line[--n] = '\0';
        if (regexec(c->re, line, 0, NULL, 0) == 0)
            fss_append(c, path, lineno, line);
    }
    fclose(f);
}

static void fss_recurse(FsSearchCtx *c, const char *path, int depth) {
    if (depth > 16 || c->count >= FS_SEARCH_MAX) return;
    DIR *d = opendir(path);
    if (!d) {
        /* treat as a single file */
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) fss_file(c, path);
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && c->count < FS_SEARCH_MAX) {
        if (ent->d_name[0] == '.') continue;
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        struct stat st;
        if (lstat(child, &st) < 0) continue;
        if      (S_ISDIR(st.st_mode)) fss_recurse(c, child, depth + 1);
        else if (S_ISREG(st.st_mode)) fss_file(c, child);
    }
    closedir(d);
}

/* ── fs.search ───────────────────────────────────────────────────────*/
static void tool_fs_search(int fd, const char *json) {
    char path[512] = ".", pattern[256] = "";

    json_str(json, "path",    path,    sizeof(path));
    if (!json_str(json, "pattern", pattern, sizeof(pattern)) || !pattern[0]) {
        send_error(fd, "fs.search", "missing 'pattern'"); return;
    }
    if (!path_is_safe(path)) {
        send_error(fd, "fs.search", "path traversal rejected"); return;
    }

    regex_t re;
    if (regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB) != 0) {
        send_error(fd, "fs.search", "invalid regex pattern"); return;
    }

    char *buf = malloc(131072);
    if (!buf) { regfree(&re); send_error(fd, "fs.search", "out of memory"); return; }

    int hdr = snprintf(buf, 131072,
                       "{\"status\":\"ok\",\"tool\":\"fs.search\",\"results\":[");
    buf[hdr] = '\0';

    FsSearchCtx ctx = { &re, buf, 131072, (size_t)hdr, 0, 1 };
    fss_recurse(&ctx, path, 0);
    regfree(&re);

    snprintf(ctx.buf + ctx.buf_len,
             ctx.buf_size - ctx.buf_len,
             "],\"count\":%d}", ctx.count);
    send_framed(fd, buf);
    free(buf);
    aishell_log(LOG_MCP, "fs.search", pattern, "ok");
}

/* ── Week 10: proc.* MCP handlers ──────────────────────────────────── */

static void tool_proc_list(int fd) {
    char *arr = malloc(8192);
    if (!arr) { send_error(fd, "proc.list", "out of memory"); return; }
    proc_list_json(arr, 8192);
    /* arr is a bare JSON array; wrap in the standard response envelope. */
    size_t rlen = strlen(arr) + 64;
    char  *resp = malloc(rlen);
    if (!resp) { free(arr); send_error(fd, "proc.list", "out of memory"); return; }
    snprintf(resp, rlen,
             "{\"status\":\"ok\",\"tool\":\"proc.list\",\"result\":%s}", arr);
    free(arr);
    send_framed(fd, resp);
    free(resp);
}

static void tool_proc_kill(int fd, const char *json) {
    char id_s[32]  = {0};
    char sig_s[32] = "SIGTERM";
    json_str(json, "job_id", id_s,  sizeof(id_s));
    json_str(json, "signal", sig_s, sizeof(sig_s));
    if (!id_s[0]) { send_error(fd, "proc.kill", "missing 'job_id'"); return; }
    int  job_id = atoi(id_s);
    char resp[256];
    proc_kill_json(job_id, sig_s, resp, sizeof(resp));
    send_framed(fd, resp);
}

static void tool_proc_wait(int fd, const char *json) {
    char id_s[32] = {0};
    json_str(json, "job_id", id_s, sizeof(id_s));
    if (!id_s[0]) { send_error(fd, "proc.wait", "missing 'job_id'"); return; }
    int  job_id = atoi(id_s);
    char resp[256];
    proc_wait_json(job_id, resp, sizeof(resp));
    send_framed(fd, resp);
}

/* ── Week 10: env.* MCP handlers ───────────────────────────────────── */

/* NOTE: setenv() modifies the process-wide environ table — all threads
 * (REPL, MCP server, SIGCHLD reaper) see the change immediately.
 * mcp_sync_var() additionally updates the REPL's private var_store so
 * that $NAME expansion via var_get() is not shadowed by a stale entry. */

extern char **environ;

/* Validate that name matches [A-Za-z_][A-Za-z0-9_]* to block injection. */
static int is_valid_varname(const char *name) {
    if (!name || !*name) return 0;
    if (!( (*name >= 'A' && *name <= 'Z') ||
           (*name >= 'a' && *name <= 'z') ||
            *name == '_' )) return 0;
    for (const char *p = name + 1; *p; p++) {
        if (!( (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
               (*p >= '0' && *p <= '9') || *p == '_' )) return 0;
    }
    return 1;
}

static void tool_env_get(int fd, const char *json) {
    char name[128] = {0};
    if (!json_str(json, "name", name, sizeof(name)) || !name[0]) {
        send_error(fd, "env.get", "missing 'name'"); return;
    }
    char ename[256];
    json_escape(name, ename, sizeof(ename));
    const char *val = getenv(name);
    char resp[2048];
    if (val) {
        char eval[1536];
        json_escape(val, eval, sizeof(eval));
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"ok\",\"tool\":\"env.get\","
                 "\"result\":{\"name\":\"%s\",\"value\":\"%s\"}}",
                 ename, eval);
    } else {
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"ok\",\"tool\":\"env.get\","
                 "\"result\":{\"name\":\"%s\",\"value\":null}}",
                 ename);
    }
    send_framed(fd, resp);
}

static void tool_env_set(int fd, const char *json) {
    char name[128] = {0}, value[1024] = {0};
    if (!json_str(json, "name",  name,  sizeof(name))  || !name[0]) {
        send_error(fd, "env.set", "missing 'name'"); return;
    }
    if (!json_str(json, "value", value, sizeof(value))) {
        send_error(fd, "env.set", "missing 'value'"); return;
    }
    if (!is_valid_varname(name)) {
        send_error(fd, "env.set", "invalid variable name"); return;
    }
    if (setenv(name, value, 1) < 0) {
        send_error(fd, "env.set", strerror(errno)); return;
    }
    mcp_sync_var(name, value);   /* keep REPL var_store consistent */

    char ename[256], evalue[1024];
    json_escape(name,  ename,  sizeof(ename));
    json_escape(value, evalue, sizeof(evalue));
    char resp[1536];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"tool\":\"env.set\","
             "\"result\":{\"name\":\"%s\",\"value\":\"%s\"}}",
             ename, evalue);
    send_framed(fd, resp);
}

static void tool_env_list(int fd) {
    /* Build JSON object from the process environ table.
     * Cap at 200 entries to bound response size. */
    size_t cap  = 65536;
    char  *buf  = malloc(cap);
    if (!buf) { send_error(fd, "env.list", "out of memory"); return; }

    int off = snprintf(buf, cap,
                       "{\"status\":\"ok\",\"tool\":\"env.list\",\"result\":{");
    int first = 1, count = 0;
    for (char **ep = environ; *ep && count < 200; ep++, count++) {
        const char *eq = strchr(*ep, '=');
        if (!eq) continue;
        size_t nlen = (size_t)(eq - *ep);
        char   rawname[256];
        if (nlen >= sizeof(rawname)) continue;
        memcpy(rawname, *ep, nlen);
        rawname[nlen] = '\0';
        const char *rawval = eq + 1;
        char ename[512], evalue[512];
        json_escape(rawname, ename,  sizeof(ename));
        json_escape(rawval,  evalue, sizeof(evalue));
        int elen = snprintf(buf + off, cap - (size_t)off,
                            "%s\"%s\":\"%s\"",
                            first ? "" : ",", ename, evalue);
        if (off + elen + 8 >= (int)cap) break;
        off += elen;
        first = 0;
    }
    snprintf(buf + off, cap - (size_t)off, "}}");
    send_framed(fd, buf);
    free(buf);
}

/* ── Week 10: edit.* MCP handlers ──────────────────────────────────── */

static void tool_edit_replace_line(int fd, const char *json) {
    char path[512] = {0}, line_s[32] = {0}, text[4096] = {0};
    if (!json_str(json, "path", path, sizeof(path)) || !path[0])
        { send_error(fd, "edit.replace_line", "missing 'path'"); return; }
    if (!json_str(json, "line", line_s, sizeof(line_s)) || !line_s[0])
        { send_error(fd, "edit.replace_line", "missing 'line'"); return; }
    if (!json_str(json, "text", text, sizeof(text)))
        { send_error(fd, "edit.replace_line", "missing 'text'"); return; }
    int n = atoi(line_s);
    char errbuf[256] = "";
    if (edit_op_replace_line(path, n, text, errbuf, sizeof(errbuf)) < 0) {
        send_error(fd, "edit.replace_line", errbuf); return;
    }
    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"tool\":\"edit.replace_line\","
             "\"result\":{\"line\":%d}}", n);
    send_framed(fd, resp);
}

static void tool_edit_insert_line(int fd, const char *json) {
    char path[512] = {0}, line_s[32] = {0}, text[4096] = {0};
    if (!json_str(json, "path", path, sizeof(path)) || !path[0])
        { send_error(fd, "edit.insert_line", "missing 'path'"); return; }
    if (!json_str(json, "line", line_s, sizeof(line_s)) || !line_s[0])
        { send_error(fd, "edit.insert_line", "missing 'line'"); return; }
    if (!json_str(json, "text", text, sizeof(text)))
        { send_error(fd, "edit.insert_line", "missing 'text'"); return; }
    int n = atoi(line_s);
    char errbuf[256] = "";
    if (edit_op_insert_line(path, n, text, errbuf, sizeof(errbuf)) < 0) {
        send_error(fd, "edit.insert_line", errbuf); return;
    }
    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"tool\":\"edit.insert_line\","
             "\"result\":{\"inserted_before_line\":%d}}", n);
    send_framed(fd, resp);
}

static void tool_edit_delete_line(int fd, const char *json) {
    char path[512] = {0}, line_s[32] = {0};
    if (!json_str(json, "path", path, sizeof(path)) || !path[0])
        { send_error(fd, "edit.delete_line", "missing 'path'"); return; }
    if (!json_str(json, "line", line_s, sizeof(line_s)) || !line_s[0])
        { send_error(fd, "edit.delete_line", "missing 'line'"); return; }
    int n = atoi(line_s);
    char errbuf[256] = "";
    if (edit_op_delete_line(path, n, errbuf, sizeof(errbuf)) < 0) {
        send_error(fd, "edit.delete_line", errbuf); return;
    }
    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"tool\":\"edit.delete_line\","
             "\"result\":{\"deleted_line\":%d}}", n);
    send_framed(fd, resp);
}

static void tool_edit_replace(int fd, const char *json) {
    char path[512] = {0}, pattern[1024] = {0}, replacement[1024] = {0};
    if (!json_str(json, "path",        path,        sizeof(path))        || !path[0])
        { send_error(fd, "edit.replace", "missing 'path'");        return; }
    if (!json_str(json, "pattern",     pattern,     sizeof(pattern))     || !pattern[0])
        { send_error(fd, "edit.replace", "missing 'pattern'");     return; }
    if (!json_str(json, "replacement", replacement, sizeof(replacement)))
        { send_error(fd, "edit.replace", "missing 'replacement'"); return; }
    int  nrep   = 0;
    char errbuf[256] = "";
    if (edit_op_replace(path, pattern, replacement, &nrep, errbuf, sizeof(errbuf)) < 0) {
        send_error(fd, "edit.replace", errbuf); return;
    }
    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"tool\":\"edit.replace\","
             "\"result\":{\"replacements\":%d}}", nrep);
    send_framed(fd, resp);
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
    /* ── Week 10: fs.* filesystem tools ── */
    else if (strcmp(tool, "fs.list")      == 0) tool_fs_list(fd, first_line);
    else if (strcmp(tool, "fs.read")      == 0) tool_fs_read(fd, first_line);
    else if (strcmp(tool, "fs.write")     == 0) tool_fs_write(fd, first_line);
    else if (strcmp(tool, "fs.append")    == 0) tool_fs_append(fd, first_line);
    else if (strcmp(tool, "fs.stat")      == 0) tool_fs_stat(fd, first_line);
    else if (strcmp(tool, "fs.search")    == 0) tool_fs_search(fd, first_line);
    /* ── Week 10: proc.* process management tools ── */
    else if (strcmp(tool, "proc.list")    == 0) tool_proc_list(fd);
    else if (strcmp(tool, "proc.kill")    == 0) tool_proc_kill(fd, first_line);
    else if (strcmp(tool, "proc.wait")    == 0) tool_proc_wait(fd, first_line);
    /* ── Week 10: env.* environment tools ── */
    else if (strcmp(tool, "env.get")          == 0) tool_env_get(fd, first_line);
    else if (strcmp(tool, "env.set")          == 0) tool_env_set(fd, first_line);
    else if (strcmp(tool, "env.list")         == 0) tool_env_list(fd);
    /* ── Week 10: edit.* text editing tools ── */
    else if (strcmp(tool, "edit.replace_line")== 0) tool_edit_replace_line(fd, first_line);
    else if (strcmp(tool, "edit.insert_line") == 0) tool_edit_insert_line(fd, first_line);
    else if (strcmp(tool, "edit.delete_line") == 0) tool_edit_delete_line(fd, first_line);
    else if (strcmp(tool, "edit.replace")     == 0) tool_edit_replace(fd, first_line);
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
