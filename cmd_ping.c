#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stddef.h>
#include <time.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

/* ── ICMP types and packet layout ─────────────────────────────────────── */
#define ICMP_ECHO_REQUEST  8
#define ICMP_ECHO_REPLY    0
#define ICMP_DATA_SZ      56   /* 56 data + 8 header = 64 bytes total */

/* Fields laid out so no padding is inserted (all fields naturally aligned). */
typedef struct {
    uint8_t  type, code;
    uint16_t cksum, id, seq;
    char     data[ICMP_DATA_SZ];
} icmp_pkt_t;

static uint16_t icmp_cksum(const void *d, size_t n)
{
    const uint16_t *p = d;
    uint32_t s = 0;
    while (n > 1) { s += *p++; n -= 2; }
    if (n) s += *(const uint8_t *)p;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

static double mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void sleep_s(int s) {
    struct timespec ts = { .tv_sec = s, .tv_nsec = 0 };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ; /* retry on signal */
}

/* ── Argtable ─────────────────────────────────────────────────────────── */
static void build_ping_argtable(arg_lit_t **help, arg_lit_t **help_json,
                                 arg_int_t **count, arg_int_t **wait_t,
                                 arg_int_t **interval, arg_lit_t **quiet,
                                 arg_str_t **host, arg_end_t **end,
                                 void ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json", "print machine-readable help as JSON");
    *count     = arg_int0("c", "count",    "N", "stop after N packets (default 4)");
    *wait_t    = arg_int0("W", "timeout",  "N", "seconds to wait per reply (default 1)");
    *interval  = arg_int0("i", "interval", "N", "seconds between packets (default 1)");
    *quiet     = arg_lit0("q", "quiet",        "suppress per-packet output");
    *host      = arg_str1(NULL, NULL,      "HOST", "hostname or IPv4 address");
    *end       = arg_end(20);

    static void *argtable[9];
    argtable[0] = *help;     argtable[1] = *help_json;
    argtable[2] = *count;    argtable[3] = *wait_t;
    argtable[4] = *interval; argtable[5] = *quiet;
    argtable[6] = *host;     argtable[7] = *end;
    argtable[8] = NULL;
    *argtable_out = argtable;
}

void ping_print_usage(FILE *out);
extern cmd_spec_t cmd_ping_spec;

int ping_run(int argc, char **argv)
{
    arg_lit_t *help, *help_json, *quiet;
    arg_int_t *count, *wait_t, *interval;
    arg_str_t *host;
    arg_end_t *end;
    void     **argtable;

    build_ping_argtable(&help, &help_json, &count, &wait_t, &interval,
                        &quiet, &host, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 8);
        ping_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_ping_spec, argtable, stdout);
        arg_freetable(argtable, 8);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "ping");
        arg_freetable(argtable, 8);
        ping_print_usage(stdout);
        return 2;
    }

    int  npack      = (count->count    > 0) ? count->ival[0]    : 4;
    int  timeout_s  = (wait_t->count   > 0) ? wait_t->ival[0]   : 1;
    int  interval_s = (interval->count > 0) ? interval->ival[0] : 1;
    int  do_quiet   = (quiet->count    > 0);
    char hostbuf[256];
    strncpy(hostbuf, host->sval[0], sizeof(hostbuf) - 1);
    arg_freetable(argtable, 8);

    /* ── Resolve hostname ── */
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_RAW;
    int gai = getaddrinfo(hostbuf, NULL, &hints, &res);
    if (gai != 0 || !res) {
        fprintf(stderr, "ping: %s: %s\n", hostbuf, gai_strerror(gai));
        return 2;
    }
    struct sockaddr_in dst;
    memcpy(&dst, res->ai_addr, sizeof(dst));
    freeaddrinfo(res);

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &dst.sin_addr, ip_str, sizeof(ip_str));

    /* ── Open raw socket ── */
    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) {
        if (errno == EPERM || errno == EACCES) {
            /* Fall back: fork + exec /bin/ping */
            fprintf(stderr,
                    "ping: no raw-socket privilege (CAP_NET_RAW); "
                    "trying /bin/ping\n");
            pid_t child = fork();
            if (child == 0) {
                char cnt_s[16];
                snprintf(cnt_s, sizeof(cnt_s), "%d", npack);
                execl("/bin/ping", "ping", "-c", cnt_s, hostbuf, (char *)NULL);
                _exit(127);
            } else if (child > 0) {
                int st = 0;
                waitpid(child, &st, 0);
                return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
            }
        }
        perror("ping: socket");
        return 2;
    }

    /* Set per-reply timeout via SO_RCVTIMEO */
    struct timeval tv = { .tv_sec = timeout_s, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint16_t pid16 = (uint16_t)getpid();
    int sent = 0, received = 0;
    double rtt_min = 1e9, rtt_max = 0.0, rtt_sum = 0.0;
    double t_start = mono_ms();

    printf("PING %s (%s): %d bytes of data.\n",
           hostbuf, ip_str, ICMP_DATA_SZ);

    for (int seq = 1; seq <= npack; seq++) {
        /* Build and send ICMP ECHO_REQUEST */
        icmp_pkt_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.type  = ICMP_ECHO_REQUEST;
        pkt.id    = htons(pid16);
        pkt.seq   = htons((uint16_t)seq);
        memset(pkt.data, 0x41, sizeof(pkt.data));   /* fill with 'A' */
        pkt.cksum = icmp_cksum(&pkt, sizeof(pkt));

        double t_send = mono_ms();
        if (sendto(sock, &pkt, sizeof(pkt), 0,
                   (struct sockaddr *)&dst, sizeof(dst)) < 0) {
            perror("ping: sendto");
            break;
        }
        sent++;

        /* Receive replies until we get our echo-reply or timeout */
        char recvbuf[1024];
        int  got = 0;
        while (!got) {
            struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);
            ssize_t r = recvfrom(sock, recvbuf, sizeof(recvbuf), 0,
                                 (struct sockaddr *)&from, &fromlen);
            if (r < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (!do_quiet)
                        printf("Request timeout for icmp_seq %d\n", seq);
                } else {
                    perror("ping: recvfrom");
                }
                break;
            }
            /* Skip if too short for a minimal IP+ICMP header */
            if (r < 28) continue;

            /* Parse IP header length (IHL field, units = 32-bit words) */
            int   iphdr_len = (recvbuf[0] & 0x0f) * 4;
            uint8_t ttl     = (uint8_t)recvbuf[8];

            if (r < iphdr_len + (int)sizeof(icmp_pkt_t)) continue;
            icmp_pkt_t *rp = (icmp_pkt_t *)(recvbuf + iphdr_len);

            if (rp->type != ICMP_ECHO_REPLY)           continue;
            if (ntohs(rp->id)  != pid16)               continue;
            if (ntohs(rp->seq) != (uint16_t)seq)       continue;

            double rtt = mono_ms() - t_send;
            received++;
            if (rtt < rtt_min) rtt_min = rtt;
            if (rtt > rtt_max) rtt_max = rtt;
            rtt_sum += rtt;
            got = 1;

            if (!do_quiet) {
                char from_s[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &from.sin_addr, from_s, sizeof(from_s));
                printf("%d bytes from %s: icmp_seq=%d ttl=%d time=%.3f ms\n",
                       (int)(r - iphdr_len), from_s, seq, (int)ttl, rtt);
            }
        }

        if (seq < npack) sleep_s(interval_s);
    }

    double elapsed = mono_ms() - t_start;
    printf("\n--- %s ping statistics ---\n", hostbuf);
    printf("%d packets transmitted, %d received, %.0f%% packet loss, "
           "time %.0fms\n",
           sent, received,
           sent > 0 ? (double)(sent - received) / sent * 100.0 : 0.0,
           elapsed);
    if (received > 0)
        printf("rtt min/avg/max = %.3f/%.3f/%.3f ms\n",
               rtt_min, rtt_sum / received, rtt_max);

    close(sock);
    return (received > 0) ? 0 : 1;
}

void ping_print_usage(FILE *out)
{
    arg_lit_t *help, *help_json, *quiet;
    arg_int_t *count, *wait_t, *interval;
    arg_str_t *host;
    arg_end_t *end;
    void     **argtable;

    build_ping_argtable(&help, &help_json, &count, &wait_t, &interval,
                        &quiet, &host, &end, &argtable);
    fprintf(out, "Usage: ping ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nSend ICMP ECHO_REQUEST to HOST and report round-trip times.\n");
    fprintf(out, "Requires CAP_NET_RAW or root; falls back to /bin/ping on EPERM.\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-28s %s\n");
    arg_freetable(argtable, 8);
}

cmd_spec_t cmd_ping_spec = {
    .name      = "ping",
    .summary   = "send ICMP ECHO_REQUEST packets to a host",
    .long_help = "Send COUNT ICMP ECHO_REQUEST packets to HOST and report "
                 "round-trip time, TTL, and packet-loss statistics. "
                 "Requires CAP_NET_RAW or root privileges; falls back to "
                 "/bin/ping when the raw socket cannot be created.",
    .run         = ping_run,
    .print_usage = ping_print_usage,
};

void register_ping_command(void) {
    register_command(&cmd_ping_spec);
}
