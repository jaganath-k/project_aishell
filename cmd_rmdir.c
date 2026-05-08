#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

static void build_rmdir_argtable(arg_lit_t  **help,
                                  arg_lit_t  **help_json,
                                  arg_lit_t  **parents,
                                  arg_file_t **dirs,
                                  arg_end_t  **end,
                                  void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json", "print machine-readable help as JSON");
    *parents   = arg_lit0("p", "parents",   "remove DIR and its ancestors (e.g. a/b/c removes c, b, a)");
    *dirs      = arg_filen(NULL, NULL, "DIR...", 1, 64, "directories to remove");
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

void rmdir_print_usage(FILE *out);
extern cmd_spec_t cmd_rmdir_spec;

/* Remove directory then each ancestor working upward. */
static int rmdir_parents(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);

    /* Strip trailing slashes. */
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/')
        tmp[--len] = '\0';

    while (len > 0) {
        if (rmdir(tmp) != 0) {
            fprintf(stderr, "rmdir: failed to remove '%s': %s\n",
                    tmp, strerror(errno));
            return 1;
        }
        char *sep = strrchr(tmp, '/');
        if (!sep)
            break;
        *sep = '\0';
        len  = (size_t)(sep - tmp);
    }
    return 0;
}

int rmdir_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json, *parents;
    arg_file_t *dirs;
    arg_end_t  *end;
    void      **argtable;

    build_rmdir_argtable(&help, &help_json, &parents, &dirs, &end, &argtable);

    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 5);
        rmdir_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_rmdir_spec, argtable, stdout);
        arg_freetable(argtable, 5);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "rmdir");
        arg_freetable(argtable, 5);
        rmdir_print_usage(stdout);
        return 1;
    }

    int par = (parents->count > 0);
    int ret = 0;

    for (int i = 0; i < dirs->count; i++) {
        const char *dir = dirs->filename[i];
        if (par) {
            if (rmdir_parents(dir) != 0)
                ret = 1;
        } else {
            if (rmdir(dir) != 0) {
                fprintf(stderr, "rmdir: failed to remove '%s': %s\n",
                        dir, strerror(errno));
                ret = 1;
            }
        }
    }

    arg_freetable(argtable, 5);
    return ret;
}

void rmdir_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json, *parents;
    arg_file_t *dirs;
    arg_end_t  *end;
    void      **argtable;

    build_rmdir_argtable(&help, &help_json, &parents, &dirs, &end, &argtable);

    fprintf(out, "Usage: rmdir ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-20s %s\n");

    arg_freetable(argtable, 5);
}

cmd_spec_t cmd_rmdir_spec = {
    .name      = "rmdir",
    .summary   = "remove empty directories",
    .long_help = "Remove each DIR if it is empty. "
                 "With -p, also remove each ancestor directory up the path.",
    .run         = rmdir_run,
    .print_usage = rmdir_print_usage,
};

void register_rmdir_command(void) {
    register_command(&cmd_rmdir_spec);
}
