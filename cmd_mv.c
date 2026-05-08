#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <limits.h>
#include <unistd.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

static void build_mv_argtable(arg_lit_t  **help,
                               arg_lit_t  **help_json,
                               arg_lit_t  **interactive,
                               arg_file_t **files,
                               arg_end_t  **end,
                               void       ***argtable_out)
{
    *help        = arg_lit0("h", "help",        "show help and exit");
    *help_json   = arg_lit0(NULL, "help-json",  "print machine-readable help as JSON");
    *interactive = arg_lit0("i", "interactive", "prompt before overwrite");
    *files       = arg_filen(NULL, NULL, "SOURCE... DEST", 2, 65,
                             "source file(s) and destination");
    *end         = arg_end(20);

    static void *argtable[6];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *interactive;
    argtable[3] = *files;
    argtable[4] = *end;
    argtable[5] = NULL;

    *argtable_out = argtable;
}

void mv_print_usage(FILE *out);
extern cmd_spec_t cmd_mv_spec;

static const char *base_name(const char *path) {
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

/* Minimal file copy used for cross-device moves. */
static int mv_copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) {
        fprintf(stderr, "mv: cannot open '%s': %s\n", src, strerror(errno));
        return 1;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fprintf(stderr, "mv: cannot create '%s': %s\n", dst, strerror(errno));
        fclose(in);
        return 1;
    }

    char buf[65536];
    size_t n;
    int ret = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fprintf(stderr, "mv: write error to '%s': %s\n", dst, strerror(errno));
            ret = 1;
            break;
        }
    }
    if (!ret && ferror(in)) {
        fprintf(stderr, "mv: read error from '%s': %s\n", src, strerror(errno));
        ret = 1;
    }
    fclose(in);
    fclose(out);

    struct stat st;
    if (!ret && stat(src, &st) == 0)
        chmod(dst, st.st_mode & 0777);

    return ret;
}

static int move_entry(const char *src, const char *dst, int interactive) {
    /* If dst is a directory, move src inside it. */
    char final_dst[PATH_MAX];
    struct stat dst_st;
    if (stat(dst, &dst_st) == 0 && S_ISDIR(dst_st.st_mode))
        snprintf(final_dst, sizeof(final_dst), "%s/%s", dst, base_name(src));
    else
        snprintf(final_dst, sizeof(final_dst), "%s", dst);

    if (interactive) {
        struct stat st;
        if (stat(final_dst, &st) == 0) {
            fprintf(stderr, "mv: overwrite '%s'? ", final_dst);
            fflush(stderr);
            char buf[8];
            if (!fgets(buf, sizeof(buf), stdin) ||
                (buf[0] != 'y' && buf[0] != 'Y'))
                return 0;
        }
    }

    /* Try atomic rename first (works within same filesystem). */
    if (rename(src, final_dst) == 0)
        return 0;

    /* Cross-device: copy file then remove source. */
    if (errno == EXDEV) {
        struct stat src_st;
        if (lstat(src, &src_st) != 0) {
            fprintf(stderr, "mv: cannot stat '%s': %s\n", src, strerror(errno));
            return 1;
        }
        if (S_ISDIR(src_st.st_mode)) {
            fprintf(stderr,
                    "mv: cannot move directory '%s' across filesystems\n", src);
            return 1;
        }
        if (mv_copy_file(src, final_dst) != 0)
            return 1;
        if (unlink(src) != 0) {
            fprintf(stderr, "mv: cannot remove '%s': %s\n", src, strerror(errno));
            return 1;
        }
        return 0;
    }

    fprintf(stderr, "mv: cannot move '%s' to '%s': %s\n",
            src, final_dst, strerror(errno));
    return 1;
}

int mv_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json, *interactive;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_mv_argtable(&help, &help_json, &interactive, &files, &end, &argtable);

    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 5);
        mv_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_mv_spec, argtable, stdout);
        arg_freetable(argtable, 5);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "mv");
        arg_freetable(argtable, 5);
        mv_print_usage(stdout);
        return 1;
    }

    int inter = (interactive->count > 0);
    int count = files->count;
    const char *dst = files->filename[count - 1];

    /* Multiple sources require dst to be a directory. */
    if (count > 2) {
        struct stat st;
        if (stat(dst, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "mv: target '%s' is not a directory\n", dst);
            arg_freetable(argtable, 5);
            return 1;
        }
    }

    int ret = 0;
    for (int i = 0; i < count - 1; i++) {
        if (move_entry(files->filename[i], dst, inter) != 0)
            ret = 1;
    }

    arg_freetable(argtable, 5);
    return ret;
}

void mv_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json, *interactive;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_mv_argtable(&help, &help_json, &interactive, &files, &end, &argtable);

    fprintf(out, "Usage: mv ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-20s %s\n");

    arg_freetable(argtable, 5);
}

cmd_spec_t cmd_mv_spec = {
    .name      = "mv",
    .summary   = "move or rename files",
    .long_help = "Rename SOURCE to DEST, or move SOURCE(s) into a directory DEST.",
    .run         = mv_run,
    .print_usage = mv_print_usage,
};

void register_mv_command(void) {
    register_command(&cmd_mv_spec);
}
