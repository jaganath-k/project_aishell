#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <limits.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

static void build_cp_argtable(arg_lit_t  **help,
                               arg_lit_t  **help_json,
                               arg_lit_t  **recursive,
                               arg_lit_t  **interactive,
                               arg_file_t **files,
                               arg_end_t  **end,
                               void       ***argtable_out)
{
    *help        = arg_lit0("h", "help",        "show help and exit");
    *help_json   = arg_lit0(NULL, "help-json",  "print machine-readable help as JSON");
    *recursive   = arg_lit0("r", "recursive",   "copy directories recursively");
    *interactive = arg_lit0("i", "interactive", "prompt before overwrite");
    *files       = arg_filen(NULL, NULL, "SOURCE... DEST", 2, 65,
                             "source file(s) and destination");
    *end         = arg_end(20);

    static void *argtable[7];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *recursive;
    argtable[3] = *interactive;
    argtable[4] = *files;
    argtable[5] = *end;
    argtable[6] = NULL;

    *argtable_out = argtable;
}

void cp_print_usage(FILE *out);
extern cmd_spec_t cmd_cp_spec;

static const char *base_name(const char *path) {
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

static int prompt_overwrite(const char *cmd, const char *path) {
    fprintf(stderr, "%s: overwrite '%s'? ", cmd, path);
    fflush(stderr);
    char buf[8];
    return fgets(buf, sizeof(buf), stdin) && (buf[0] == 'y' || buf[0] == 'Y');
}

static int copy_file(const char *src, const char *dst, int interactive) {
    struct stat dst_st;
    if (interactive && stat(dst, &dst_st) == 0)
        if (!prompt_overwrite("cp", dst))
            return 0;

    FILE *in = fopen(src, "rb");
    if (!in) {
        fprintf(stderr, "cp: cannot open '%s': %s\n", src, strerror(errno));
        return 1;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fprintf(stderr, "cp: cannot create '%s': %s\n", dst, strerror(errno));
        fclose(in);
        return 1;
    }

    char buf[65536];
    size_t n;
    int ret = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fprintf(stderr, "cp: write error to '%s': %s\n", dst, strerror(errno));
            ret = 1;
            break;
        }
    }
    if (!ret && ferror(in)) {
        fprintf(stderr, "cp: read error from '%s': %s\n", src, strerror(errno));
        ret = 1;
    }
    fclose(in);
    fclose(out);

    struct stat src_st;
    if (!ret && stat(src, &src_st) == 0)
        chmod(dst, src_st.st_mode & 0777);

    return ret;
}

static int copy_recursive(const char *src, const char *dst, int interactive);

static int copy_recursive(const char *src, const char *dst, int interactive) {
    struct stat st;
    if (lstat(src, &st) != 0) {
        fprintf(stderr, "cp: cannot stat '%s': %s\n", src, strerror(errno));
        return 1;
    }

    if (S_ISDIR(st.st_mode)) {
        struct stat dst_st;
        if (stat(dst, &dst_st) != 0) {
            if (mkdir(dst, st.st_mode & 0777) != 0) {
                fprintf(stderr, "cp: cannot create directory '%s': %s\n",
                        dst, strerror(errno));
                return 1;
            }
        }

        DIR *dir = opendir(src);
        if (!dir) {
            fprintf(stderr, "cp: cannot open directory '%s': %s\n",
                    src, strerror(errno));
            return 1;
        }

        int ret = 0;
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                continue;
            char src_path[PATH_MAX], dst_path[PATH_MAX];
            snprintf(src_path, sizeof(src_path), "%s/%s", src, entry->d_name);
            snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, entry->d_name);
            if (copy_recursive(src_path, dst_path, interactive) != 0)
                ret = 1;
        }
        closedir(dir);
        return ret;
    }

    return copy_file(src, dst, interactive);
}

static int copy_entry(const char *src, const char *dst,
                      int recursive, int interactive) {
    struct stat src_st;
    if (lstat(src, &src_st) != 0) {
        fprintf(stderr, "cp: cannot stat '%s': %s\n", src, strerror(errno));
        return 1;
    }

    /* If dst is a directory, copy src inside it. */
    char final_dst[PATH_MAX];
    struct stat dst_st;
    if (stat(dst, &dst_st) == 0 && S_ISDIR(dst_st.st_mode))
        snprintf(final_dst, sizeof(final_dst), "%s/%s", dst, base_name(src));
    else
        snprintf(final_dst, sizeof(final_dst), "%s", dst);

    if (S_ISDIR(src_st.st_mode)) {
        if (!recursive) {
            fprintf(stderr, "cp: omitting directory '%s' — use -r\n", src);
            return 1;
        }
        return copy_recursive(src, final_dst, interactive);
    }

    return copy_file(src, final_dst, interactive);
}

int cp_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json, *recursive, *interactive;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_cp_argtable(&help, &help_json, &recursive, &interactive,
                      &files, &end, &argtable);

    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 6);
        cp_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_cp_spec, argtable, stdout);
        arg_freetable(argtable, 6);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "cp");
        arg_freetable(argtable, 6);
        cp_print_usage(stdout);
        return 1;
    }

    int rec   = (recursive->count   > 0);
    int inter = (interactive->count > 0);
    int count = files->count;
    const char *dst = files->filename[count - 1];

    /* Multiple sources require dst to be a directory. */
    if (count > 2) {
        struct stat st;
        if (stat(dst, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "cp: target '%s' is not a directory\n", dst);
            arg_freetable(argtable, 6);
            return 1;
        }
    }

    int ret = 0;
    for (int i = 0; i < count - 1; i++) {
        if (copy_entry(files->filename[i], dst, rec, inter) != 0)
            ret = 1;
    }

    arg_freetable(argtable, 6);
    return ret;
}

void cp_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json, *recursive, *interactive;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_cp_argtable(&help, &help_json, &recursive, &interactive,
                      &files, &end, &argtable);

    fprintf(out, "Usage: cp ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-20s %s\n");

    arg_freetable(argtable, 6);
}

cmd_spec_t cmd_cp_spec = {
    .name      = "cp",
    .summary   = "copy files and directories",
    .long_help = "Copy SOURCE to DEST, or multiple SOURCE(s) into a directory DEST.",
    .run         = cp_run,
    .print_usage = cp_print_usage,
};

void register_cp_command(void) {
    register_command(&cmd_cp_spec);
}
