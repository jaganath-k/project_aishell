#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

static void build_mkdir_argtable(arg_lit_t  **help,
                                  arg_lit_t  **help_json,
                                  arg_lit_t  **parents,
                                  arg_file_t **dirs,
                                  arg_end_t  **end,
                                  void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",    "show help and exit");
    *help_json = arg_lit0(NULL, "help-json", "print machine-readable help as JSON");
    *parents   = arg_lit0("p", "parents", "create parent directories as needed, no error if exists");
    *dirs      = arg_filen(NULL, NULL, "DIR...", 1, 64, "directories to create");
    *end       = arg_end(20);

    static void *argtable[6];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *parents;
    argtable[3] = *dirs;
    argtable[4] = *end;
    argtable[5] = NULL;

    *argtable_out = argtable;
}

void mkdir_print_usage(FILE *out);
extern cmd_spec_t cmd_mkdir_spec;

/* Create all path components, ignoring EEXIST at each level. */
static int mkdir_parents(const char *path, mode_t mode) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
                fprintf(stderr, "mkdir: cannot create '%s': %s\n",
                        tmp, strerror(errno));
                return 1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir: cannot create '%s': %s\n",
                tmp, strerror(errno));
        return 1;
    }
    return 0;
}

int mkdir_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json, *parents;
    arg_file_t *dirs;
    arg_end_t  *end;
    void      **argtable;

    build_mkdir_argtable(&help, &help_json, &parents, &dirs, &end, &argtable);

    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 5);
        mkdir_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_mkdir_spec, argtable, stdout);
        arg_freetable(argtable, 5);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "mkdir");
        arg_freetable(argtable, 5);
        mkdir_print_usage(stdout);
        return 1;
    }

    int  par = (parents->count > 0);
    int  ret = 0;
    mode_t mode = 0755;

    for (int i = 0; i < dirs->count; i++) {
        const char *dir = dirs->filename[i];
        if (par) {
            if (mkdir_parents(dir, mode) != 0)
                ret = 1;
        } else {
            if (mkdir(dir, mode) != 0) {
                fprintf(stderr, "mkdir: cannot create '%s': %s\n",
                        dir, strerror(errno));
                ret = 1;
            }
        }
    }

    arg_freetable(argtable, 5);
    return ret;
}

void mkdir_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json, *parents;
    arg_file_t *dirs;
    arg_end_t  *end;
    void      **argtable;

    build_mkdir_argtable(&help, &help_json, &parents, &dirs, &end, &argtable);

    fprintf(out, "Usage: mkdir ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-20s %s\n");

    arg_freetable(argtable, 5);
}

cmd_spec_t cmd_mkdir_spec = {
    .name      = "mkdir",
    .summary   = "make directories",
    .long_help = "Create each DIR. With -p, create parent directories as needed "
                 "and do not error if the directory already exists.",
    .run         = mkdir_run,
    .print_usage = mkdir_print_usage,
};

void register_mkdir_command(void) {
    register_command(&cmd_mkdir_spec);
}
