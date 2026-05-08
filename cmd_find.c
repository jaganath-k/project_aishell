#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fnmatch.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

/* Options passed through the recursive walk */
typedef struct {
    const char *name_pat;   /* NULL = no filter */
    int         type_filter;/* 0 = any, 'f' = file, 'd' = dir, 'l' = symlink */
    int         maxdepth;   /* -1 = unlimited */
} FindOpts;

/* Portable basename (last path component) without modifying the string */
static const char *path_basename(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static void do_find(const char *path, const FindOpts *opts, int depth) {
    struct stat st;
    if (lstat(path, &st) < 0) {
        fprintf(stderr, "find: %s: %s\n", path, strerror(errno));
        return;
    }

    /* Determine entry type */
    int is_dir  = S_ISDIR(st.st_mode);
    int is_file = S_ISREG(st.st_mode);
    int is_link = S_ISLNK(st.st_mode);

    /* Check type filter */
    int type_ok = 1;
    if (opts->type_filter == 'f') type_ok = is_file;
    else if (opts->type_filter == 'd') type_ok = is_dir;
    else if (opts->type_filter == 'l') type_ok = is_link;

    /* Check name pattern */
    int name_ok = 1;
    if (opts->name_pat)
        name_ok = (fnmatch(opts->name_pat, path_basename(path), 0) == 0);

    if (type_ok && name_ok)
        puts(path);

    /* Recurse into directories */
    if (!is_dir) return;
    if (opts->maxdepth >= 0 && depth >= opts->maxdepth) return;

    DIR *dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "find: %s: %s\n", path, strerror(errno));
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char child[PATH_MAX];
        if (strcmp(path, ".") == 0)
            snprintf(child, sizeof(child), "./%s", ent->d_name);
        else
            snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);

        do_find(child, opts, depth + 1);
    }
    closedir(dir);
}

static void build_find_argtable(arg_lit_t **help,
                                 arg_lit_t **help_json,
                                 arg_str_t **path,
                                 arg_str_t **name_pat,
                                 arg_str_t **type_filter,
                                 arg_int_t **maxdepth,
                                 arg_end_t **end,
                                 void      ***argtable_out)
{
    *help        = arg_lit0("h", "help",      "show help and exit");
    *help_json   = arg_lit0(NULL, "help-json","print machine-readable help as JSON");
    *path        = arg_str0(NULL, NULL, "PATH","starting directory (default: .)");
    *name_pat    = arg_str0("n", "name", "PATTERN",
                            "match filename against shell glob (e.g. '*.c')");
    *type_filter = arg_str0("t", "type", "TYPE",
                            "filter by type: f=regular file, d=directory, l=symlink");
    *maxdepth    = arg_int0(NULL, "maxdepth", "N",
                            "descend at most N directory levels (0 = starting path only)");
    *end         = arg_end(20);

    static void *argtable[8];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *path;
    argtable[3] = *name_pat;
    argtable[4] = *type_filter;
    argtable[5] = *maxdepth;
    argtable[6] = *end;
    argtable[7] = NULL;
    *argtable_out = argtable;
}

void find_print_usage(FILE *out);
extern cmd_spec_t cmd_find_spec;

int find_run(int argc, char **argv) {
    arg_lit_t *help, *help_json;
    arg_str_t *path, *name_pat, *type_filter;
    arg_int_t *maxdepth;
    arg_end_t *end;
    void     **argtable;

    build_find_argtable(&help, &help_json, &path, &name_pat,
                        &type_filter, &maxdepth, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 7);
        find_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_find_spec, argtable, stdout);
        arg_freetable(argtable, 7);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "find");
        arg_freetable(argtable, 7);
        find_print_usage(stdout);
        return 1;
    }

    FindOpts opts;
    opts.name_pat = (name_pat->count > 0)   ? name_pat->sval[0]   : NULL;
    opts.maxdepth = (maxdepth->count > 0)   ? maxdepth->ival[0]   : -1;

    /* Validate -t TYPE */
    opts.type_filter = 0;
    if (type_filter->count > 0) {
        const char *tv = type_filter->sval[0];
        if (strcmp(tv, "f") == 0)      opts.type_filter = 'f';
        else if (strcmp(tv, "d") == 0) opts.type_filter = 'd';
        else if (strcmp(tv, "l") == 0) opts.type_filter = 'l';
        else {
            fprintf(stderr, "find: unknown type '%s' (use f, d, or l)\n", tv);
            arg_freetable(argtable, 7);
            return 1;
        }
    }

    const char *start = (path->count > 0) ? path->sval[0] : ".";
    arg_freetable(argtable, 7);

    do_find(start, &opts, 0);
    return 0;
}

void find_print_usage(FILE *out) {
    arg_lit_t *help, *help_json;
    arg_str_t *path, *name_pat, *type_filter;
    arg_int_t *maxdepth;
    arg_end_t *end;
    void     **argtable;

    build_find_argtable(&help, &help_json, &path, &name_pat,
                        &type_filter, &maxdepth, &end, &argtable);
    fprintf(out, "Usage: find ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nRecursively search PATH for entries matching the given filters.\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-32s %s\n");
    arg_freetable(argtable, 7);
}

cmd_spec_t cmd_find_spec = {
    .name      = "find",
    .summary   = "search for files in a directory hierarchy",
    .long_help = "Walk the directory tree rooted at PATH (default: .) and print "
                 "each matching entry. Use -n for shell-glob name filtering, "
                 "-t to restrict by type (f/d/l), and --maxdepth to limit recursion depth.",
    .run         = find_run,
    .print_usage = find_print_usage,
};

void register_find_command(void) {
    register_command(&cmd_find_spec);
}
