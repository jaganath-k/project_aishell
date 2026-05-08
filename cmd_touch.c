#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

static void build_touch_argtable(arg_lit_t  **help,
                                  arg_lit_t  **help_json,
                                  arg_file_t **files,
                                  arg_end_t  **end,
                                  void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json", "print machine-readable help as JSON");
    *files     = arg_filen(NULL, NULL, "FILE...", 1, 64, "files to touch");
    *end       = arg_end(20);

    static void *argtable[5];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *files;
    argtable[3] = *end;
    argtable[4] = NULL;

    *argtable_out = argtable;
}

void touch_print_usage(FILE *out);
extern cmd_spec_t cmd_touch_spec;

static int touch_file(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        /* File exists — update timestamps to now. */
        if (utimensat(AT_FDCWD, path, NULL, 0) != 0) {
            fprintf(stderr, "touch: cannot update '%s': %s\n",
                    path, strerror(errno));
            return 1;
        }
    } else {
        /* File does not exist — create it empty. */
        FILE *f = fopen(path, "a");
        if (!f) {
            fprintf(stderr, "touch: cannot create '%s': %s\n",
                    path, strerror(errno));
            return 1;
        }
        fclose(f);
    }
    return 0;
}

int touch_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_touch_argtable(&help, &help_json, &files, &end, &argtable);

    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 4);
        touch_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_touch_spec, argtable, stdout);
        arg_freetable(argtable, 4);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "touch");
        arg_freetable(argtable, 4);
        touch_print_usage(stdout);
        return 1;
    }

    int ret = 0;
    for (int i = 0; i < files->count; i++) {
        if (touch_file(files->filename[i]) != 0)
            ret = 1;
    }

    arg_freetable(argtable, 4);
    return ret;
}

void touch_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_touch_argtable(&help, &help_json, &files, &end, &argtable);

    fprintf(out, "Usage: touch ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-20s %s\n");

    arg_freetable(argtable, 4);
}

cmd_spec_t cmd_touch_spec = {
    .name      = "touch",
    .summary   = "update file timestamps",
    .long_help = "Update the access and modification timestamps of each FILE to now. "
                 "Create the file if it does not exist.",
    .run         = touch_run,
    .print_usage = touch_print_usage,
};

void register_touch_command(void) {
    register_command(&cmd_touch_spec);
}
