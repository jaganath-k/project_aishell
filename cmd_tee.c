#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

static void build_tee_argtable(arg_lit_t  **help,
                                arg_lit_t  **help_json,
                                arg_lit_t  **append,
                                arg_file_t **files,
                                arg_end_t  **end,
                                void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",   "show help and exit");
    *help_json = arg_lit0(NULL, "help-json", "print machine-readable help as JSON");
    *append    = arg_lit0("a", "append", "append to files instead of overwriting");
    *files     = arg_filen(NULL, NULL, "FILE", 1, 64, "output file(s)");
    *end       = arg_end(20);

    static void *argtable[6];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *append;
    argtable[3] = *files;
    argtable[4] = *end;
    argtable[5] = NULL;
    *argtable_out = argtable;
}

void tee_print_usage(FILE *out);
extern cmd_spec_t cmd_tee_spec;

int tee_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json, *append;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_tee_argtable(&help, &help_json, &append, &files, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 5);
        tee_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_tee_spec, argtable, stdout);
        arg_freetable(argtable, 5);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "tee");
        arg_freetable(argtable, 5);
        tee_print_usage(stdout);
        return 1;
    }

    int do_append = (append->count > 0);
    int nfiles    = files->count;
    int oflags    = O_WRONLY | O_CREAT | (do_append ? O_APPEND : O_TRUNC);

    /* Open all output files upfront */
    int *fds = malloc((size_t)nfiles * sizeof(int));
    if (!fds) { arg_freetable(argtable, 5); return 1; }

    int ret = 0;
    for (int i = 0; i < nfiles; i++) {
        fds[i] = open(files->filename[i], oflags, 0644);
        if (fds[i] < 0) {
            fprintf(stderr, "tee: %s: %s\n", files->filename[i], strerror(errno));
            fds[i] = -1;
            ret = 1;
        }
    }

    arg_freetable(argtable, 5);

    /* Read stdin → stdout + each file */
    char buf[4096];
    ssize_t n;
    while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        /* Write to stdout first */
        if (write(STDOUT_FILENO, buf, (size_t)n) < 0) {
            ret = 1;
            break;
        }
        /* Write to each file */
        for (int i = 0; i < nfiles; i++) {
            if (fds[i] < 0) continue;
            if (write(fds[i], buf, (size_t)n) < 0) {
                fprintf(stderr, "tee: write error: %s\n", strerror(errno));
                ret = 1;
            }
        }
    }

    for (int i = 0; i < nfiles; i++)
        if (fds[i] >= 0) close(fds[i]);
    free(fds);
    return ret;
}

void tee_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json, *append;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_tee_argtable(&help, &help_json, &append, &files, &end, &argtable);
    fprintf(out, "Usage: tee ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nRead stdin and write to stdout and FILE(s) simultaneously.\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-28s %s\n");
    arg_freetable(argtable, 5);
}

cmd_spec_t cmd_tee_spec = {
    .name      = "tee",
    .summary   = "read stdin and write to stdout and files",
    .long_help = "Copy stdin to stdout and to each FILE simultaneously. "
                 "With -a, append to files instead of overwriting. "
                 "Useful for inspecting pipeline data mid-stream: "
                 "cmd | tee out.txt | next-cmd",
    .run         = tee_run,
    .print_usage = tee_print_usage,
};

void register_tee_command(void) {
    register_command(&cmd_tee_spec);
}
