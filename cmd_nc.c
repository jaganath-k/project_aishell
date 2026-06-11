#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/types.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

/* ── Bidirectional relay: stdin ↔ sock ───────────────────────────────── */
/* Exits when either side closes.  If timeout_s > 0, idle timeout applies. */
static void relay(int sock, int timeout_s)
{
    signal(SIGPIPE, SIG_IGN);
    char    buf[4096];
    int     stdin_open = 1;

    for (;;) {
        fd_set rd;
        FD_ZERO(&rd);
        if (stdin_open) FD_SET(STDIN_FILENO, &rd);
        FD_SET(sock, &rd);

        struct timeval tv  = { .tv_sec = timeout_s, .tv_usec = 0 };
        struct timeval *tp = (timeout_s > 0) ? &tv : NULL;

        int n = select(sock + 1, &rd, NULL, NULL, tp);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) break;   /* idle timeout */

        if (stdin_open && FD_ISSET(STDIN_FILENO, &rd)) {
            ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
            if (r <= 0) {
                stdin_open = 0;
                shutdown(sock, SHUT_WR);   /* send FIN to peer */
            } else {
                if (write(sock, buf, (size_t)r) < 0) break;
            }
        }

        if (FD_ISSET(sock, &rd)) {
            ssize_t r = read(sock, buf, sizeof(buf));
            if (r <= 0) break;   /* peer closed */
            if (write(STDOUT_FILENO, buf, (size_t)r) < 0) break;
        }
    }
}

/* ── Argtable ─────────────────────────────────────────────────────────── */
static void build_nc_argtable(arg_lit_t **help, arg_lit_t **help_json,
                               arg_lit_t **lst, arg_int_t **p_port,
                               arg_lit_t **zero,   arg_lit_t **verbose,
                               arg_int_t **wait,
                               arg_str_t **addrs,  arg_end_t **end,
                               void ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json", "print machine-readable help as JSON");
    *lst       = arg_lit0("l", "listen",    "listen mode: accept one TCP connection");
    *p_port    = arg_int0("p", "port",  "PORT", "port to listen on (-l) or connect to");
    *zero      = arg_lit0("z", "zero-io",   "port-scan mode: connect and close immediately");
    *verbose   = arg_lit0("v", "verbose",   "print connection status to stderr");
    *wait      = arg_int0("w", "wait",  "N",    "idle timeout in seconds");
    *addrs     = arg_strn(NULL, NULL, "ADDR", 0, 2,
                          "HOST PORT (client) or PORT (listen)");
    *end       = arg_end(20);

    static void *argtable[10];
    argtable[0] = *help;    argtable[1] = *help_json;
    argtable[2] = *lst;     argtable[3] = *p_port;
    argtable[4] = *zero;    argtable[5] = *verbose;
    argtable[6] = *wait;    argtable[7] = *addrs;
    argtable[8] = *end;     argtable[9] = NULL;
    *argtable_out = argtable;
}

void nc_print_usage(FILE *out);
extern cmd_spec_t cmd_nc_spec;

/* ── nc_run ───────────────────────────────────────────────────────────── */
int nc_run(int argc, char **argv)
{
    arg_lit_t *help, *help_json, *lst, *zero, *verbose;
    arg_int_t *p_port, *wait;
    arg_str_t *addrs;
    arg_end_t *end;
    void     **argtable;

    build_nc_argtable(&help, &help_json, &lst, &p_port, &zero, &verbose,
                      &wait, &addrs, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 9); nc_print_usage(stdout); return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_nc_spec, argtable, stdout);
        arg_freetable(argtable, 9); return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "nc");
        arg_freetable(argtable, 9); nc_print_usage(stdout); return 2;
    }

    int  do_listen  = (lst->count  > 0);
    int  do_zero    = (zero->count    > 0);
    int  do_verbose = (verbose->count > 0);
    int  timeout_s  = (wait->count    > 0) ? wait->ival[0] : 0;
    int  port       = (p_port->count  > 0) ? p_port->ival[0] : 0;
    const char *host = NULL;

    /* Resolve HOST and PORT from positional args */
    if (addrs->count == 2) {
        host = addrs->sval[0];
        port = atoi(addrs->sval[1]);
    } else if (addrs->count == 1) {
        if (do_listen)
            port = atoi(addrs->sval[0]);   /* nc -l PORT */
        else {
            host = addrs->sval[0];         /* port must come from -p */
        }
    }

    /* Copy strings before freeing argtable */
    char hostbuf[256] = "";
    if (host) strncpy(hostbuf, host, sizeof(hostbuf) - 1);
    arg_freetable(argtable, 9);

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "nc: invalid or missing port\n");
        return 2;
    }

    /* ── Listen mode ─────────────────────────────────────────────────── */
    if (do_listen) {
        int lsock = socket(AF_INET, SOCK_STREAM, 0);
        if (lsock < 0) { perror("nc: socket"); return 2; }
        int opt = 1;
        setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family      = AF_INET;
        sa.sin_port        = htons((uint16_t)port);
        sa.sin_addr.s_addr = INADDR_ANY;

        if (bind(lsock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
            perror("nc: bind"); close(lsock); return 2;
        }
        if (listen(lsock, 1) < 0) {
            perror("nc: listen"); close(lsock); return 2;
        }
        if (do_verbose)
            fprintf(stderr, "nc: listening on 0.0.0.0:%d\n", port);

        struct sockaddr_in peer;
        socklen_t peerlen = sizeof(peer);
        int conn = accept(lsock, (struct sockaddr *)&peer, &peerlen);
        close(lsock);
        if (conn < 0) { perror("nc: accept"); return 2; }

        if (do_verbose) {
            char ps[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &peer.sin_addr, ps, sizeof(ps));
            fprintf(stderr, "nc: connection from %s:%d\n",
                    ps, ntohs(peer.sin_port));
        }
        relay(conn, timeout_s);
        close(conn);
        return 0;
    }

    /* ── Client / zero-io mode ───────────────────────────────────────── */
    if (!hostbuf[0]) {
        fprintf(stderr, "nc: missing HOST\n");
        return 2;
    }

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    int gai = getaddrinfo(hostbuf, port_str, &hints, &res);
    if (gai != 0 || !res) {
        fprintf(stderr, "nc: %s: %s\n", hostbuf, gai_strerror(gai));
        return 2;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) { perror("nc: socket"); freeaddrinfo(res); return 2; }

    /* Optional connect timeout via SO_SNDTIMEO */
    if (timeout_s > 0) {
        struct timeval tv = { .tv_sec = timeout_s, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        if (do_verbose)
            fprintf(stderr, "nc: connect to %s port %d: %s\n",
                    hostbuf, port, strerror(errno));
        close(sock);
        freeaddrinfo(res);
        return 1;
    }

    if (do_verbose) {
        char ip_s[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr,
                  ip_s, sizeof(ip_s));
        fprintf(stderr, "nc: connection to %s %d port [tcp] succeeded\n",
                ip_s, port);
    }
    freeaddrinfo(res);

    if (do_zero) {
        /* Port scan: connection succeeded → open */
        close(sock);
        return 0;
    }

    /* Full relay */
    relay(sock, timeout_s);
    close(sock);
    return 0;
}

/* ── Usage / spec ─────────────────────────────────────────────────────── */
void nc_print_usage(FILE *out)
{
    arg_lit_t *help, *help_json, *lst, *zero, *verbose;
    arg_int_t *p_port, *wait;
    arg_str_t *addrs;
    arg_end_t *end;
    void     **argtable;

    build_nc_argtable(&help, &help_json, &lst, &p_port, &zero, &verbose,
                      &wait, &addrs, &end, &argtable);
    fprintf(out, "Usage: nc ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nTCP client, listener, and port-scanner.\n");
    fprintf(out, "  nc HOST PORT        connect to HOST on PORT, relay stdin/stdout\n");
    fprintf(out, "  nc -l PORT          listen on PORT, relay stdin/stdout\n");
    fprintf(out, "  nc -z HOST PORT     check if PORT is open (zero I/O)\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-28s %s\n");
    arg_freetable(argtable, 9);
}

cmd_spec_t cmd_nc_spec = {
    .name      = "nc",
    .summary   = "TCP client, server, and port scanner (netcat)",
    .long_help = "Open TCP connections, listen for incoming connections, or "
                 "scan ports. In client mode relay stdin to the socket and "
                 "socket output to stdout. Use -z for non-destructive port "
                 "scanning. Use -l to accept one inbound connection.",
    .run         = nc_run,
    .print_usage = nc_print_usage,
};

void register_nc_command(void) {
    register_command(&cmd_nc_spec);
}
