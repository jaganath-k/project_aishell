#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <limits.h>
#include <unistd.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

static void build_rm_argtable(arg_lit_t  **help,
                               arg_lit_t  **help_json,
                               arg_lit_t  **recursive,
                               arg_lit_t  **interactive,
                               arg_lit_t  **force,
                               arg_file_t **files,
                               arg_end_t  **end,
                               void       ***argtable_out)
{
    *help        = arg_lit0("h", "help",        "show help and exit");
    *help_json   = arg_lit0(NULL, "help-json",  "print machine-readable help as JSON");
    *recursive   = arg_lit0("r", "recursive",   "remove directories and their contents");
    *interactive = arg_lit0("i", "interactive", "prompt before every removal");
    *force       = arg_lit0("f", "force",       "ignore nonexistent files, no errors");
    *files       = arg_filen(NULL, NULL, "FILE...", 1, 64, "files or directories to remove");
    *end         = arg_end(20);

    static void *argtable[8];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *recursive;
    argtable[3] = *interactive;
    argtable[4] = *force;
    argtable[5] = *files;
    argtable[6] = *end;
    argtable[7] = NULL;

    *argtable_out = argtable;
}

void rm_print_usage(FILE *out);
extern cmd_spec_t cmd_rm_spec;

static int remove_entry(const char *path, int recursive,
                        int interactive, int force) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (!force)
            fprintf(stderr, "rm: cannot remove '%s': %s\n",
                    path, strerror(errno));
        return force ? 0 : 1;
    }

    if (S_ISDIR(st.st_mode)) {
        if (!recursive) {
            fprintf(stderr,
                    "rm: cannot remove '%s': Is a directory (use -r)\n", path);
            return 1;
        }

        DIR *dir = opendir(path);
        if (!dir) {
            fprintf(stderr, "rm: cannot open '%s': %s\n",
                    path, strerror(errno));
            return 1;
        }

        int ret = 0;
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                continue;
            char subpath[PATH_MAX];
            snprintf(subpath, sizeof(subpath), "%s/%s", path, entry->d_name);
            if (remove_entry(subpath, recursive, interactive, force) != 0)
                ret = 1;
        }
        closedir(dir);

        if (ret != 0) return ret;

        if (interactive) {
            fprintf(stderr, "rm: remove directory '%s'? ", path);
            fflush(stderr);
            char buf[8];
            if (!fgets(buf, sizeof(buf), stdin) ||
                (buf[0] != 'y' && buf[0] != 'Y'))
                return 0;
        }

        if (rmdir(path) != 0) {
            fprintf(stderr, "rm: cannot remove directory '%s': %s\n",
                    path, strerror(errno));
            return 1;
        }
        return 0;
    }

    /* Regular file or symlink. */
    if (interactive) {
        fprintf(stderr, "rm: remove '%s'? ", path);
        fflush(stderr);
        char buf[8];
        if (!fgets(buf, sizeof(buf), stdin) ||
            (buf[0] != 'y' && buf[0] != 'Y'))
            return 0;
    }

    if (unlink(path) != 0) {
        if (!force)
            fprintf(stderr, "rm: cannot remove '%s': %s\n",
                    path, strerror(errno));
        return force ? 0 : 1;
    }
    return 0;
}

int rm_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json, *recursive, *interactive, *force;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_rm_argtable(&help, &help_json, &recursive, &interactive,
                      &force, &files, &end, &argtable);

    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 7);
        rm_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_rm_spec, argtable, stdout);
        arg_freetable(argtable, 7);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "rm");
        arg_freetable(argtable, 7);
        rm_print_usage(stdout);
        return 1;
    }

    int rec   = (recursive->count   > 0);
    int inter = (interactive->count > 0);
    int frc   = (force->count       > 0);
    int ret   = 0;

    for (int i = 0; i < files->count; i++) {
        if (remove_entry(files->filename[i], rec, inter, frc) != 0)
            ret = 1;
    }

    arg_freetable(argtable, 7);
    return ret;
}

void rm_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json, *recursive, *interactive, *force;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_rm_argtable(&help, &help_json, &recursive, &interactive,
                      &force, &files, &end, &argtable);

    fprintf(out, "Usage: rm ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-20s %s\n");

    arg_freetable(argtable, 7);
}

cmd_spec_t cmd_rm_spec = {
    .name      = "rm",
    .summary   = "remove files or directories",
    .long_help = "Remove each FILE. Directories require -r to remove recursively.",
    .run         = rm_run,
    .print_usage = rm_print_usage,
};

void register_rm_command(void) {
    register_command(&cmd_rm_spec);
}
