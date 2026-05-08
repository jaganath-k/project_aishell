#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/types.h>
#include <unistd.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

/* -------------------------------------------------------------------------
 * Minimal /proc reader (session-filtered variant)
 * ---------------------------------------------------------------------- */

typedef struct {
    int  pid;
    int  ppid;
    int  session;
    char state;
    char cmdline[1024];
    char name[256];
} job_t;

static int read_job(int pid, job_t *j) {
    char path[64], buf[2048], line[512];
    FILE *f;

    memset(j, 0, sizeof(*j));
    j->pid   = pid;
    j->state = '?';

    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Name:\t", 6) == 0) {
            strncpy(j->name, line + 6, sizeof(j->name) - 1);
            j->name[strcspn(j->name, "\n")] = '\0';
        } else if (strncmp(line, "State:\t", 7) == 0) {
            j->state = line[7];
        } else {
            sscanf(line, "PPid:\t%d", &j->ppid);
        }
    }
    fclose(f);

    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    f = fopen(path, "r");
    if (f) {
        int n = (int)fread(j->cmdline, 1, sizeof(j->cmdline) - 1, f);
        fclose(f);
        for (int i = 0; i < n; i++)
            if (j->cmdline[i] == '\0') j->cmdline[i] = ' ';
        j->cmdline[n] = '\0';
        while (n > 0 && j->cmdline[n - 1] == ' ') j->cmdline[--n] = '\0';
    }
    if (j->cmdline[0] == '\0')
        snprintf(j->cmdline, sizeof(j->cmdline), "[%s]", j->name);

    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    f = fopen(path, "r");
    if (f) {
        if (fgets(buf, sizeof(buf), f)) {
            char *q = strrchr(buf, ')');
            if (q) {
                char st; int ppid2, pgrp, session;
                if (sscanf(q + 1, " %c %d %d %d", &st, &ppid2, &pgrp, &session) == 4)
                    j->session = session;
            }
        }
        fclose(f);
    }
    return 0;
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

static void build_jobs_argtable(arg_lit_t  **help,
                                 arg_lit_t  **help_json,
                                 arg_lit_t  **json_out,
                                 arg_end_t  **end,
                                 void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json","print machine-readable help as JSON");
    *json_out  = arg_lit0(NULL, "json",     "output in JSON format");
    *end       = arg_end(20);

    static void *argtable[5];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *json_out;
    argtable[3] = *end;
    argtable[4] = NULL;

    *argtable_out = argtable;
}

void jobs_print_usage(FILE *out);
extern cmd_spec_t cmd_jobs_spec;

/* -------------------------------------------------------------------------
 * jobs_run
 * ---------------------------------------------------------------------- */

int jobs_run(int argc, char **argv) {
    arg_lit_t *help, *help_json, *json_out;
    arg_end_t *end;
    void     **argtable;

    build_jobs_argtable(&help, &help_json, &json_out, &end, &argtable);

    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 4);
        jobs_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_jobs_spec, argtable, stdout);
        arg_freetable(argtable, 4);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "jobs");
        arg_freetable(argtable, 4);
        jobs_print_usage(stdout);
        return 1;
    }

    int json = (json_out->count > 0);
    arg_freetable(argtable, 4);

    /* Show processes that share our terminal session, excluding self. */
    int mysid = (int)getsid(0);
    int mypid = (int)getpid();

    DIR *d = opendir("/proc");
    if (!d) {
        fprintf(stderr, "jobs: cannot open /proc: %s\n", strerror(errno));
        return 1;
    }

    job_t *jobs = NULL;
    int njobs = 0, cap = 0;

    struct dirent *ent;
    while ((ent = readdir(d))) {
        int pid = 0, ok = 1;
        for (const char *c = ent->d_name; *c; c++) {
            if (*c < '0' || *c > '9') { ok = 0; break; }
            pid = pid * 10 + (*c - '0');
        }
        if (!ok || pid <= 0 || pid == mypid) continue;

        job_t j;
        if (read_job(pid, &j) != 0) continue;
        if (j.session != mysid) continue;

        if (njobs == cap) {
            cap = cap ? cap * 2 : 16;
            job_t *tmp = realloc(jobs, (size_t)cap * sizeof(*jobs));
            if (!tmp) { fprintf(stderr, "jobs: out of memory\n"); closedir(d); free(jobs); return 1; }
            jobs = tmp;
        }
        jobs[njobs++] = j;
    }
    closedir(d);

    /* Sort by PID */
    for (int i = 1; i < njobs; i++) {
        job_t t = jobs[i];
        int k = i - 1;
        while (k >= 0 && jobs[k].pid > t.pid) { jobs[k+1] = jobs[k]; k--; }
        jobs[k+1] = t;
    }

    for (int i = 0; i < njobs; i++) {
        job_t *j = &jobs[i];
        if (json) {
            printf("{\"job\":%d,\"pid\":%d,\"ppid\":%d,\"state\":\"%c\","
                   "\"command\":\"", i + 1, j->pid, j->ppid, j->state);
            json_str(j->cmdline);
            printf("\"}\n");
        } else {
            printf("[%d] %6d %c  %s\n", i + 1, j->pid, j->state, j->cmdline);
        }
    }

    if (njobs == 0 && !json)
        printf("no jobs in current session\n");

    free(jobs);
    return 0;
}

/* -------------------------------------------------------------------------
 * jobs_print_usage
 * ---------------------------------------------------------------------- */

void jobs_print_usage(FILE *out) {
    arg_lit_t *help, *help_json, *json_out;
    arg_end_t *end;
    void     **argtable;

    build_jobs_argtable(&help, &help_json, &json_out, &end, &argtable);

    fprintf(out, "Usage: jobs ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-20s %s\n");

    arg_freetable(argtable, 4);
}

cmd_spec_t cmd_jobs_spec = {
    .name      = "jobs",
    .summary   = "list processes in current session",
    .long_help = "List all processes that share the current terminal session (same SID). "
                 "Use --json for machine-readable output.",
    .run         = jobs_run,
    .print_usage = jobs_print_usage,
};

void register_jobs_command(void) {
    register_command(&cmd_jobs_spec);
}
