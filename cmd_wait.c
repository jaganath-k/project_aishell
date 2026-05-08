#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/types.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

static void build_wait_argtable(arg_lit_t  **help,
                                 arg_lit_t  **help_json,
                                 arg_int_t  **pids,
                                 arg_end_t  **end,
                                 void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json","print machine-readable help as JSON");
    *pids      = arg_intn(NULL, NULL, "PID", 0, 64,
                          "PIDs to wait for (default: all child processes)");
    *end       = arg_end(20);

    static void *argtable[5];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *pids;
    argtable[3] = *end;
    argtable[4] = NULL;

    *argtable_out = argtable;
}

void wait_print_usage(FILE *out);
extern cmd_spec_t cmd_wait_spec;

int wait_run(int argc, char **argv) {
    arg_lit_t *help, *help_json;
    arg_int_t *pids;
    arg_end_t *end;
    void     **argtable;

    build_wait_argtable(&help, &help_json, &pids, &end, &argtable);

    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 4);
        wait_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_wait_spec, argtable, stdout);
        arg_freetable(argtable, 4);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "wait");
        arg_freetable(argtable, 4);
        wait_print_usage(stdout);
        return 1;
    }

    int ret = 0;

    if (pids->count == 0) {
        /* Wait for all children */
        int status;
        pid_t p;
        while ((p = waitpid(-1, &status, 0)) > 0) {
            if (WIFEXITED(status))
                ret = WEXITSTATUS(status);
            else if (WIFSIGNALED(status))
                ret = 128 + WTERMSIG(status);
            printf("wait: pid %d finished (status %d)\n", (int)p, ret);
        }
        if (errno != ECHILD) {
            fprintf(stderr, "wait: %s\n", strerror(errno));
            arg_freetable(argtable, 4);
            return 1;
        }
    } else {
        for (int i = 0; i < pids->count; i++) {
            pid_t p = (pid_t)pids->ival[i];
            int status;
            if (waitpid(p, &status, 0) < 0) {
                fprintf(stderr, "wait: %d: %s\n", (int)p, strerror(errno));
                ret = 1;
                continue;
            }
            if (WIFEXITED(status))
                ret = WEXITSTATUS(status);
            else if (WIFSIGNALED(status))
                ret = 128 + WTERMSIG(status);
            printf("wait: pid %d finished (status %d)\n", (int)p, ret);
        }
    }

    arg_freetable(argtable, 4);
    return ret;
}

void wait_print_usage(FILE *out) {
    arg_lit_t *help, *help_json;
    arg_int_t *pids;
    arg_end_t *end;
    void     **argtable;

    build_wait_argtable(&help, &help_json, &pids, &end, &argtable);

    fprintf(out, "Usage: wait ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-20s %s\n");

    arg_freetable(argtable, 4);
}

cmd_spec_t cmd_wait_spec = {
    .name      = "wait",
    .summary   = "wait for process completion",
    .long_help = "Wait for each PID to complete and report its exit status. "
                 "With no PIDs, waits for all child processes of the current shell.",
    .run         = wait_run,
    .print_usage = wait_print_usage,
};

void register_wait_command(void) {
    register_command(&cmd_wait_spec);
}
