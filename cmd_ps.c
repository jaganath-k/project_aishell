#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

/* -------------------------------------------------------------------------
 * /proc reader
 * ---------------------------------------------------------------------- */

typedef struct {
    int   pid;
    int   ppid;
    int   session;
    char  state;
    uid_t uid;
    char  name[256];
    char  cmdline[1024];
} proc_t;

static int read_proc(int pid, proc_t *p) {
    char path[64], buf[2048], line[512];
    FILE *f;

    memset(p, 0, sizeof(*p));
    p->pid   = pid;
    p->uid   = (uid_t)-1;
    p->state = '?';

    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Name:\t", 6) == 0) {
            strncpy(p->name, line + 6, sizeof(p->name) - 1);
            p->name[strcspn(p->name, "\n")] = '\0';
        } else if (strncmp(line, "State:\t", 7) == 0) {
            p->state = line[7];
        } else {
            sscanf(line, "PPid:\t%d", &p->ppid);
            sscanf(line, "Uid:\t%u",  &p->uid);
        }
    }
    fclose(f);

    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    f = fopen(path, "r");
    if (f) {
        int n = (int)fread(p->cmdline, 1, sizeof(p->cmdline) - 1, f);
        fclose(f);
        for (int i = 0; i < n; i++)
            if (p->cmdline[i] == '\0') p->cmdline[i] = ' ';
        p->cmdline[n] = '\0';
        while (n > 0 && p->cmdline[n - 1] == ' ') p->cmdline[--n] = '\0';
    }
    if (p->cmdline[0] == '\0')
        snprintf(p->cmdline, sizeof(p->cmdline), "[%s]", p->name);

    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    f = fopen(path, "r");
    if (f) {
        if (fgets(buf, sizeof(buf), f)) {
            char *q = strrchr(buf, ')');
            if (q) {
                char st; int ppid2, pgrp, session;
                if (sscanf(q + 1, " %c %d %d %d", &st, &ppid2, &pgrp, &session) == 4)
                    p->session = session;
            }
        }
        fclose(f);
    }
    return 0;
}

static const char *uid_name(uid_t uid) {
    struct passwd *pw = getpwuid(uid);
    return pw ? pw->pw_name : "?";
}

static void json_str(const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if      (c == '"')  fputs("\\\"", stdout);
        else if (c == '\\') fputs("\\\\", stdout);
        else if (c < 0x20)  printf("\\u%04x", c);
        else                putchar(c);
    }
}

/* -------------------------------------------------------------------------
 * Argtable
 * ---------------------------------------------------------------------- */

static void build_ps_argtable(arg_lit_t  **help,
                               arg_lit_t  **help_json,
                               arg_lit_t  **every,
                               arg_lit_t  **full,
                               arg_lit_t  **json_out,
                               arg_end_t  **end,
                               void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json","print machine-readable help as JSON");
    *every     = arg_lit0("e", "every",     "show all processes (not just current user)");
    *full      = arg_lit0("f", "full",      "full format: include PPID column");
    *json_out  = arg_lit0(NULL, "json",     "output in JSON format");
    *end       = arg_end(20);

    static void *argtable[7];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *every;
    argtable[3] = *full;
    argtable[4] = *json_out;
    argtable[5] = *end;
    argtable[6] = NULL;

    *argtable_out = argtable;
}

void ps_print_usage(FILE *out);
extern cmd_spec_t cmd_ps_spec;

/* -------------------------------------------------------------------------
 * ps_run
 * ---------------------------------------------------------------------- */

int ps_run(int argc, char **argv) {
    arg_lit_t *help, *help_json, *every, *full, *json_out;
    arg_end_t *end;
    void     **argtable;

    build_ps_argtable(&help, &help_json, &every, &full, &json_out, &end, &argtable);

    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 6);
        ps_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_ps_spec, argtable, stdout);
        arg_freetable(argtable, 6);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "ps");
        arg_freetable(argtable, 6);
        ps_print_usage(stdout);
        return 1;
    }

    int show_all  = (every->count   > 0);
    int show_full = (full->count    > 0);
    int json      = (json_out->count > 0);
    uid_t myuid   = getuid();

    arg_freetable(argtable, 6);

    DIR *d = opendir("/proc");
    if (!d) {
        fprintf(stderr, "ps: cannot open /proc: %s\n", strerror(errno));
        return 1;
    }

    proc_t *procs = NULL;
    int nprocs = 0, cap = 0;

    struct dirent *ent;
    while ((ent = readdir(d))) {
        int pid = 0, ok = 1;
        for (const char *c = ent->d_name; *c; c++) {
            if (*c < '0' || *c > '9') { ok = 0; break; }
            pid = pid * 10 + (*c - '0');
        }
        if (!ok || pid <= 0) continue;

        proc_t p;
        if (read_proc(pid, &p) != 0) continue;
        if (!show_all && p.uid != myuid) continue;

        if (nprocs == cap) {
            cap = cap ? cap * 2 : 64;
            proc_t *tmp = realloc(procs, (size_t)cap * sizeof(*procs));
            if (!tmp) { fprintf(stderr, "ps: out of memory\n"); closedir(d); free(procs); return 1; }
            procs = tmp;
        }
        procs[nprocs++] = p;
    }
    closedir(d);

    /* Sort by PID (insertion sort — small N in practice) */
    for (int i = 1; i < nprocs; i++) {
        proc_t t = procs[i];
        int j = i - 1;
        while (j >= 0 && procs[j].pid > t.pid) { procs[j+1] = procs[j]; j--; }
        procs[j+1] = t;
    }

    if (json) {
        for (int i = 0; i < nprocs; i++) {
            proc_t *p = &procs[i];
            printf("{\"pid\":%d,\"ppid\":%d,\"session\":%d,\"state\":\"%c\","
                   "\"user\":\"", p->pid, p->ppid, p->session, p->state);
            json_str(uid_name(p->uid));
            printf("\",\"command\":\"");
            json_str(p->cmdline);
            printf("\"}\n");
        }
    } else {
        if (show_full)
            printf("%6s %6s %c %-16s %s\n", "PID", "PPID", 'S', "USER", "COMMAND");
        else
            printf("%6s %c %-16s %s\n", "PID", 'S', "USER", "COMMAND");

        for (int i = 0; i < nprocs; i++) {
            proc_t *p = &procs[i];
            if (show_full)
                printf("%6d %6d %c %-16s %s\n",
                       p->pid, p->ppid, p->state, uid_name(p->uid), p->cmdline);
            else
                printf("%6d %c %-16s %s\n",
                       p->pid, p->state, uid_name(p->uid), p->cmdline);
        }
    }

    free(procs);
    return 0;
}

/* -------------------------------------------------------------------------
 * ps_print_usage
 * ---------------------------------------------------------------------- */

void ps_print_usage(FILE *out) {
    arg_lit_t *help, *help_json, *every, *full, *json_out;
    arg_end_t *end;
    void     **argtable;

    build_ps_argtable(&help, &help_json, &every, &full, &json_out, &end, &argtable);

    fprintf(out, "Usage: ps ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-20s %s\n");

    arg_freetable(argtable, 6);
}

cmd_spec_t cmd_ps_spec = {
    .name      = "ps",
    .summary   = "report process status",
    .long_help = "List running processes. By default shows only processes owned by "
                 "the current user. Use -e for all processes, -f for full format.",
    .run         = ps_run,
    .print_usage = ps_print_usage,
};

void register_ps_command(void) {
    register_command(&cmd_ps_spec);
}
