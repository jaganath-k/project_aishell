/* ftp_handler.c — FTP-style protocol handler for AiShell Week 9.
 * Implements USER / QUIT / PORT / LIST / MKD / STOR / RETR over a
 * single control connection.  Compatible with the real Linux ftp(1) client:
 *   ftp 127.0.0.1 9000
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>

#include "ftp_handler.h"

/* ── Per-session state ─────────────────────────────────────────────── */
typedef struct {
    int  ctrl_fd;
    int  data_fd;       /* -1 when not established */
    char data_ip[64];   /* from PORT command (always overridden to 127.0.0.1) */
    int  data_port;     /* from PORT command */
    char username[64];
    char cwd[512];      /* current working directory */
    int  logged_in;     /* 1 after USER command */
} FtpSession;

/* ── Send a complete FTP response line ─────────────────────────────── */
static void ftp_reply(int fd, const char *msg) {
    send(fd, msg, strlen(msg), MSG_NOSIGNAL);
}

/* ── Read one line (strips \r\n) ───────────────────────────────────── */
static ssize_t ftp_read_line(int fd, char *buf, size_t len) {
    size_t i = 0;
    while (i < len - 1) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return (ssize_t)i;
        if (c == '\n') break;
        if (c != '\r') buf[i++] = c;
    }
    buf[i] = '\0';
    return (ssize_t)i;
}

/* ── Input validation ──────────────────────────────────────────────── */
static int is_safe_filename(const char *name) {
    if (!name || name[0] == '\0')       return 0;  /* empty */
    if (strlen(name) > 255)             return 0;  /* too long */
    if (strstr(name, ".."))             return 0;  /* traversal */
    if (strchr(name, '\0') != name + strlen(name)) return 0; /* NUL inside */
    return 1;
}

static int path_escapes_cwd(const FtpSession *s, const char *path) {
    /* Reject if the resolved path doesn't start with cwd */
    char resolved[512];
    if (!realpath(path, resolved)) return 0; /* let open() handle error */
    return strncmp(resolved, s->cwd, strlen(s->cwd)) != 0;
}

/* Build an absolute path for a filename relative to session cwd */
static int build_path(const FtpSession *s, const char *name,
                      char *out, size_t outsz) {
    if (!is_safe_filename(name)) return -1;
    int n = snprintf(out, outsz, "%s/%s", s->cwd, name);
    if (n < 0 || (size_t)n >= outsz) return -1;
    return 0;
}

/* ── Open data connection to PORT-specified address ────────────────── */
static int open_data_connection(FtpSession *s) {
    if (s->data_port <= 0) return -1;

    int dfd = socket(AF_INET, SOCK_STREAM, 0);
    if (dfd < 0) return -1;

    /* 10-second connection timeout */
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(dfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(dfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)s->data_port);
    /* Always use 127.0.0.1 regardless of what PORT sent */
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(dfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(dfd);
        return -1;
    }
    return dfd;
}

/* ── FTP command handlers ───────────────────────────────────────────── */

static void cmd_user(FtpSession *s, const char *arg) {
    if (!arg || !*arg) {
        ftp_reply(s->ctrl_fd, "501 Syntax error in parameters.\r\n");
        return;
    }
    snprintf(s->username, sizeof(s->username), "%s", arg);
    s->logged_in = 1;
    char resp[128];
    snprintf(resp, sizeof(resp), "230 User %s logged in.\r\n", s->username);
    ftp_reply(s->ctrl_fd, resp);
}

/* PORT <a1,a2,a3,a4,p1,p2> */
static void cmd_port(FtpSession *s, const char *arg) {
    if (!arg || !*arg) {
        ftp_reply(s->ctrl_fd, "501 Syntax error in parameters.\r\n");
        return;
    }

    unsigned int a1,a2,a3,a4,p1,p2;
    if (sscanf(arg, "%u,%u,%u,%u,%u,%u", &a1,&a2,&a3,&a4,&p1,&p2) != 6) {
        ftp_reply(s->ctrl_fd, "501 Syntax error in parameters.\r\n");
        return;
    }

    /* All 6 octets must be 0-255 */
    if (a1>255||a2>255||a3>255||a4>255||p1>255||p2>255) {
        ftp_reply(s->ctrl_fd, "501 Invalid PORT parameters.\r\n");
        return;
    }

    int port = (int)(p1 * 256 + p2);
    /* Reject privileged ports */
    if (port <= 1023) {
        ftp_reply(s->ctrl_fd, "504 Command not implemented for that parameter.\r\n");
        return;
    }

    /* Always override IP to localhost for safety */
    snprintf(s->data_ip, sizeof(s->data_ip), "127.0.0.1");
    s->data_port = port;

    ftp_reply(s->ctrl_fd, "200 Port command successful.\r\n");
}

/* LIST [path] */
static void cmd_list(FtpSession *s, const char *arg) {
    if (!s->logged_in) {
        ftp_reply(s->ctrl_fd, "530 Please login with USER.\r\n");
        return;
    }
    if (s->data_port <= 0) {
        ftp_reply(s->ctrl_fd, "425 Use PORT first.\r\n");
        return;
    }

    /* Validate optional path arg */
    const char *target = s->cwd;
    char path_buf[512];
    if (arg && *arg) {
        if (build_path(s, arg, path_buf, sizeof(path_buf)) < 0) {
            ftp_reply(s->ctrl_fd, "550 Permission denied.\r\n");
            return;
        }
        if (path_escapes_cwd(s, path_buf)) {
            ftp_reply(s->ctrl_fd, "550 Permission denied.\r\n");
            return;
        }
        target = path_buf;
    }

    ftp_reply(s->ctrl_fd, "150 Opening data connection.\r\n");

    int dfd = open_data_connection(s);
    if (dfd < 0) {
        ftp_reply(s->ctrl_fd, "425 Can't open data connection.\r\n");
        return;
    }

    /* Run ls -la on target directory, send output over data connection */
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "ls -la %s 2>&1", target);
    FILE *fp = popen(cmd, "r");
    if (fp) {
        char buf[4096];
        size_t nr;
        while ((nr = fread(buf, 1, sizeof(buf), fp)) > 0)
            send(dfd, buf, nr, MSG_NOSIGNAL);
        pclose(fp);
    }

    close(dfd);
    s->data_port = 0; /* consume the PORT */
    ftp_reply(s->ctrl_fd, "226 Transfer complete.\r\n");
}

/* MKD <dirname> */
static void cmd_mkd(FtpSession *s, const char *arg) {
    if (!s->logged_in) {
        ftp_reply(s->ctrl_fd, "530 Please login with USER.\r\n");
        return;
    }
    if (!arg || !*arg) {
        ftp_reply(s->ctrl_fd, "500 Syntax error.\r\n");
        return;
    }

    char path[512];
    if (build_path(s, arg, path, sizeof(path)) < 0) {
        ftp_reply(s->ctrl_fd, "550 Permission denied.\r\n");
        return;
    }

    if (mkdir(path, 0755) < 0) {
        if (errno == EEXIST)
            ftp_reply(s->ctrl_fd, "550 Directory already exists.\r\n");
        else
            ftp_reply(s->ctrl_fd, "550 Could not create directory.\r\n");
        return;
    }

    char resp[640];
    snprintf(resp, sizeof(resp), "257 \"%s\" directory created.\r\n", path);
    ftp_reply(s->ctrl_fd, resp);
}

/* STOR <filename> — binary-safe upload */
static void cmd_stor(FtpSession *s, const char *arg) {
    if (!s->logged_in) {
        ftp_reply(s->ctrl_fd, "530 Please login with USER.\r\n");
        return;
    }
    if (!arg || !*arg) {
        ftp_reply(s->ctrl_fd, "500 Syntax error.\r\n");
        return;
    }
    if (s->data_port <= 0) {
        ftp_reply(s->ctrl_fd, "425 Use PORT first.\r\n");
        return;
    }

    char path[512];
    if (build_path(s, arg, path, sizeof(path)) < 0) {
        ftp_reply(s->ctrl_fd, "550 Permission denied.\r\n");
        return;
    }

    int outfd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (outfd < 0) {
        ftp_reply(s->ctrl_fd, "450 File unavailable.\r\n");
        return;
    }

    int dfd = open_data_connection(s);
    if (dfd < 0) {
        close(outfd);
        ftp_reply(s->ctrl_fd, "425 Can't open data connection.\r\n");
        return;
    }

    ftp_reply(s->ctrl_fd, "125 Data connection already open.\r\n");

    char buf[4096];
    ssize_t nr;
    while ((nr = read(dfd, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;
        while (written < nr) {
            ssize_t w = write(outfd, buf + written, (size_t)(nr - written));
            if (w < 0) goto stor_err;
            written += w;
        }
    }

stor_err:
    close(dfd);
    close(outfd);
    s->data_port = 0;
    ftp_reply(s->ctrl_fd, "226 Transfer complete.\r\n");
}

/* RETR <filename> — binary-safe download */
static void cmd_retr(FtpSession *s, const char *arg) {
    if (!s->logged_in) {
        ftp_reply(s->ctrl_fd, "530 Please login with USER.\r\n");
        return;
    }
    if (!arg || !*arg) {
        ftp_reply(s->ctrl_fd, "500 Syntax error.\r\n");
        return;
    }

    /* Validate path and verify file exists BEFORE checking PORT.
     * Security and existence checks must always reply 550 regardless of PORT. */
    char path[512];
    if (build_path(s, arg, path, sizeof(path)) < 0) {
        ftp_reply(s->ctrl_fd, "550 Permission denied.\r\n");
        return;
    }
    if (path_escapes_cwd(s, path)) {
        ftp_reply(s->ctrl_fd, "550 Permission denied.\r\n");
        return;
    }

    int infd = open(path, O_RDONLY);
    if (infd < 0) {
        ftp_reply(s->ctrl_fd, "550 File not found.\r\n");
        return;
    }

    if (s->data_port <= 0) {
        close(infd);
        ftp_reply(s->ctrl_fd, "425 Use PORT first.\r\n");
        return;
    }

    int dfd = open_data_connection(s);
    if (dfd < 0) {
        close(infd);
        ftp_reply(s->ctrl_fd, "425 Can't open data connection.\r\n");
        return;
    }

    ftp_reply(s->ctrl_fd, "150 Opening data connection.\r\n");

    char buf[4096];
    ssize_t nr;
    while ((nr = read(infd, buf, sizeof(buf))) > 0) {
        ssize_t sent = 0;
        while (sent < nr) {
            ssize_t s2 = send(dfd, buf + sent, (size_t)(nr - sent), MSG_NOSIGNAL);
            if (s2 < 0) goto retr_err;
            sent += s2;
        }
    }

retr_err:
    close(infd);
    close(dfd);
    s->data_port = 0;
    ftp_reply(s->ctrl_fd, "226 Transfer complete.\r\n");
}

/* ══════════════════ Main Session Loop ═══════════════════════════════ */

void handle_ftp_session(int ctrl_fd, const char *first_line) {
    FtpSession s;
    memset(&s, 0, sizeof(s));
    s.ctrl_fd   = ctrl_fd;
    s.data_fd   = -1;
    s.data_port = 0;
    s.logged_in = 0;

    /* Default working directory is HOME (or /tmp as fallback) */
    const char *home = getenv("HOME");
    snprintf(s.cwd, sizeof(s.cwd), "%s", home ? home : "/tmp");

    /* FTP is server-speaks-first — send the greeting banner */
    ftp_reply(ctrl_fd, "220 AiShell FTP Service (Week 9) ready.\r\n");

    /* Process first_line if anything was already read (unusual but handle it) */
    char line[1025];
    snprintf(line, sizeof(line), "%s", first_line ? first_line : "");

    for (;;) {
        /* If we have no buffered line, read the next command */
        if (line[0] == '\0') {
            ssize_t n = ftp_read_line(ctrl_fd, line, sizeof(line));
            if (n <= 0) break; /* client disconnected or timeout */
        }

        /* Reject oversized lines */
        if (strlen(line) >= 1024) {
            ftp_reply(ctrl_fd, "500 Syntax error (command too long).\r\n");
            line[0] = '\0';
            continue;
        }

        /* Parse: COMMAND [arg] */
        char cmd[32] = {0};
        char *arg    = NULL;
        char *sp     = strchr(line, ' ');
        if (sp) {
            size_t clen = (size_t)(sp - line);
            if (clen >= sizeof(cmd)) clen = sizeof(cmd) - 1;
            memcpy(cmd, line, clen);
            cmd[clen] = '\0';
            arg = sp + 1;
            while (*arg == ' ') arg++; /* strip leading spaces */
        } else {
            snprintf(cmd, sizeof(cmd), "%.*s", (int)(sizeof(cmd) - 1), line);
        }

        /* Uppercase the command */
        for (char *p = cmd; *p; p++) *p = (char)toupper((unsigned char)*p);

        /* Dispatch */
        if      (strcmp(cmd, "USER") == 0) cmd_user(&s, arg);
        else if (strcmp(cmd, "QUIT") == 0) {
            ftp_reply(ctrl_fd, "221 Goodbye.\r\n");
            break;
        }
        else if (strcmp(cmd, "PORT") == 0) cmd_port(&s, arg);
        else if (strcmp(cmd, "LIST") == 0) cmd_list(&s, arg);
        else if (strcmp(cmd, "MKD")  == 0) cmd_mkd(&s, arg);
        else if (strcmp(cmd, "STOR") == 0) cmd_stor(&s, arg);
        else if (strcmp(cmd, "RETR") == 0) cmd_retr(&s, arg);
        else if (strcmp(cmd, "NOOP") == 0) ftp_reply(ctrl_fd, "200 OK.\r\n");
        else if (strcmp(cmd, "SYST") == 0) ftp_reply(ctrl_fd, "215 UNIX Type: L8\r\n");
        else if (strcmp(cmd, "TYPE") == 0) ftp_reply(ctrl_fd, "200 Type set to I.\r\n");
        else if (strcmp(cmd, "PWD")  == 0) {
            char resp[560];
            snprintf(resp, sizeof(resp), "257 \"%s\"\r\n", s.cwd);
            ftp_reply(ctrl_fd, resp);
        }
        else {
            ftp_reply(ctrl_fd, "500 Unknown command.\r\n");
        }

        line[0] = '\0'; /* clear for next iteration */
    }
}
