#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

/* ── Options bundle ───────────────────────────────────────────────────── */

typedef struct {
    int human;
    int all_files;
    int max_depth;
} du_opts_t;

/* ── Human-readable / 1K-block formatter ─────────────────────────────── */

static void fmt_size(long long bytes, int human, char *buf, size_t bsz) {
    if (!human) {
        snprintf(buf, bsz, "%lld", (bytes + 1023LL) / 1024LL);
        return;
    }
    const char *units[] = {"B", "K", "M", "G", "T", "P"};
    double val = (double)bytes;
    int u = 0;
    while (val >= 1024.0 && u < 5) { val /= 1024.0; u++; }
    if (u == 0)
        snprintf(buf, bsz, "%.0f%s", val, units[u]);
    else if (val < 10.0)
        snprintf(buf, bsz, "%.1f%s", val, units[u]);
    else
        snprintf(buf, bsz, "%.0f%s", val, units[u]);
}

/* ── Recursive directory walker ───────────────────────────────────────── */

static long long du_walk(const char *path, int depth, const du_opts_t *opts) {
    struct stat st;
    if (lstat(path, &st) < 0) {
        fprintf(stderr, "du: cannot access '%s': %s\n", path, strerror(errno));
        return 0;
    }

    long long total = (long long)st.st_blocks * 512LL;

    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                    continue;
                char child[PATH_MAX];
                snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
                total += du_walk(child, depth + 1, opts);
            }
            closedir(d);
        } else {
            fprintf(stderr, "du: cannot read directory '%s': %s\n", path, strerror(errno));
        }
        if (depth <= opts->max_depth) {
            char buf[32];
            fmt_size(total, opts->human, buf, sizeof(buf));
            printf("%s\t%s\n", buf, path);
        }
    } else {
        /* Regular file / symlink / other */
        int print_it = (depth == 0) ||
                       (opts->all_files && depth <= opts->max_depth);
        if (print_it) {
            char buf[32];
            fmt_size(total, opts->human, buf, sizeof(buf));
            printf("%s\t%s\n", buf, path);
        }
    }

    return total;
}

/* ── Argtable ─────────────────────────────────────────────────────────── */

static void build_du_argtable(arg_lit_t  **help,
                               arg_lit_t  **help_json,
                               arg_lit_t  **human,
                               arg_lit_t  **summarize,
                               arg_lit_t  **all_files,
                               arg_int_t  **max_depth,
                               arg_file_t **paths,
                               arg_end_t  **end,
                               void       ***argtable_out)
{
    *help      = arg_lit0(NULL, "help",          "show help and exit");
    *help_json = arg_lit0(NULL, "help-json",      "print machine-readable help as JSON");
    *human     = arg_lit0("h", "human-readable", "print sizes like 4.2K, 15M, 2.3G");
    *summarize = arg_lit0("s", "summarize",      "display only a total for each argument");
    *all_files = arg_lit0("a", "all",            "write counts for all files, not just directories");
    *max_depth = arg_int0("d", "max-depth", "N", "print totals for directories at most N levels deep");
    *paths     = arg_filen(NULL, NULL, "PATH", 0, 64, "path(s) to measure (default: .)");
    *end       = arg_end(20);

    static void *argtable[9];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *human;
    argtable[3] = *summarize;
    argtable[4] = *all_files;
    argtable[5] = *max_depth;
    argtable[6] = *paths;
    argtable[7] = *end;
    argtable[8] = NULL;
    *argtable_out = argtable;
}

void du_print_usage(FILE *out);
extern cmd_spec_t cmd_du_spec;

/* ── du_run ───────────────────────────────────────────────────────────── */

int du_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json, *human, *summarize, *all_files;
    arg_int_t  *max_depth;
    arg_file_t *paths;
    arg_end_t  *end;
    void      **argtable;

    build_du_argtable(&help, &help_json, &human, &summarize, &all_files,
                      &max_depth, &paths, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 8);
        du_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_du_spec, argtable, stdout);
        arg_freetable(argtable, 8);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "du");
        arg_freetable(argtable, 8);
        du_print_usage(stdout);
        return 1;
    }

    du_opts_t opts;
    opts.human     = (human->count > 0);
    opts.all_files = (all_files->count > 0);
    if (summarize->count > 0)
        opts.max_depth = 0;
    else if (max_depth->count > 0)
        opts.max_depth = max_depth->ival[0];
    else
        opts.max_depth = INT_MAX;

    /* Copy path pointers (they point into argv — safe after freetable) */
    int npaths = paths->count;
    const char *path_arr[64];
    for (int i = 0; i < npaths; i++)
        path_arr[i] = paths->filename[i];

    arg_freetable(argtable, 8);

    if (npaths == 0) {
        du_walk(".", 0, &opts);
    } else {
        for (int i = 0; i < npaths; i++)
            du_walk(path_arr[i], 0, &opts);
    }
    return 0;
}

/* ── du_print_usage ───────────────────────────────────────────────────── */

void du_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json, *human, *summarize, *all_files;
    arg_int_t  *max_depth;
    arg_file_t *paths;
    arg_end_t  *end;
    void      **argtable;

    build_du_argtable(&help, &help_json, &human, &summarize, &all_files,
                      &max_depth, &paths, &end, &argtable);
    fprintf(out, "Usage: du ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nEstimate disk usage of each PATH and its contents.\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-32s %s\n");
    arg_freetable(argtable, 8);
}

cmd_spec_t cmd_du_spec = {
    .name      = "du",
    .summary   = "estimate file space usage",
    .long_help = "Summarize disk usage of each PATH, recursively. "
                 "Sizes are in 1K blocks by default. "
                 "Use -h for human-readable output, -s for a single total per argument, "
                 "-d N to limit directory depth, -a to include individual files.",
    .run         = du_run,
    .print_usage = du_print_usage,
};

void register_du_command(void) {
    register_command(&cmd_du_spec);
}
