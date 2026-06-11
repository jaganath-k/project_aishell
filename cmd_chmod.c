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

/* ── Symbolic mode parser ─────────────────────────────────────────────── */

static mode_t parse_symbolic(const char *spec, mode_t cur) {
    const char *p = spec;
    while (*p) {
        /* Parse WHO (ugoa) — default 'a' when none given */
        int has_u = 0, has_g = 0, has_o = 0;
        while (*p == 'u' || *p == 'g' || *p == 'o' || *p == 'a') {
            if (*p == 'u') has_u = 1;
            if (*p == 'g') has_g = 1;
            if (*p == 'o') has_o = 1;
            if (*p == 'a') { has_u = has_g = has_o = 1; }
            p++;
        }
        if (!has_u && !has_g && !has_o) has_u = has_g = has_o = 1; /* default 'a' */

        /* Parse OP (+-=) */
        if (*p != '+' && *p != '-' && *p != '=') return (mode_t)-1;
        char op = *p++;

        /* Parse PERMS (rwxXst) — may be empty for '=' to clear */
        mode_t perm_bits = 0;
        while (*p && *p != ',' && *p != '+' && *p != '-' && *p != '=') {
            char c = *p++;
            switch (c) {
                case 'r':
                    if (has_u) perm_bits |= S_IRUSR;
                    if (has_g) perm_bits |= S_IRGRP;
                    if (has_o) perm_bits |= S_IROTH;
                    break;
                case 'w':
                    if (has_u) perm_bits |= S_IWUSR;
                    if (has_g) perm_bits |= S_IWGRP;
                    if (has_o) perm_bits |= S_IWOTH;
                    break;
                case 'x':
                    if (has_u) perm_bits |= S_IXUSR;
                    if (has_g) perm_bits |= S_IXGRP;
                    if (has_o) perm_bits |= S_IXOTH;
                    break;
                case 'X':
                    /* execute only if directory or already executable */
                    if (S_ISDIR(cur) || (cur & (S_IXUSR|S_IXGRP|S_IXOTH))) {
                        if (has_u) perm_bits |= S_IXUSR;
                        if (has_g) perm_bits |= S_IXGRP;
                        if (has_o) perm_bits |= S_IXOTH;
                    }
                    break;
                case 's':
                    if (has_u) perm_bits |= S_ISUID;
                    if (has_g) perm_bits |= S_ISGID;
                    break;
                case 't':
                    perm_bits |= S_ISVTX;
                    break;
                default:
                    return (mode_t)-1;
            }
        }

        /* Build who-mask (bits '=' clears before setting new perms) */
        mode_t who_mask = 0;
        if (has_u) who_mask |= S_IRWXU | S_ISUID;
        if (has_g) who_mask |= S_IRWXG | S_ISGID;
        if (has_o) who_mask |= S_IRWXO;
        who_mask |= S_ISVTX;

        switch (op) {
            case '+': cur |= perm_bits; break;
            case '-': cur &= ~perm_bits; break;
            case '=': cur = (cur & ~who_mask) | perm_bits; break;
        }

        if (*p == ',') p++;
    }
    return cur;
}

/* ── Mode string parser (octal or symbolic) ───────────────────────────── */

static int parse_mode_str(const char *s, mode_t *out, int *is_octal) {
    size_t len = strlen(s);
    if (len >= 1 && len <= 4) {
        int all_oct = 1;
        for (size_t i = 0; i < len; i++)
            if (s[i] < '0' || s[i] > '7') { all_oct = 0; break; }
        if (all_oct) {
            *is_octal = 1;
            *out = (mode_t)strtol(s, NULL, 8);
            return 0;
        }
    }
    /* Symbolic: must start with a valid character */
    if (strchr("ugoa+-=", s[0]) == NULL) return -1;
    *is_octal = 0;
    *out = 0;
    return 0;
}

/* ── Apply chmod to one file ──────────────────────────────────────────── */

static int chmod_one(const char *path, int is_octal, mode_t octal_mode,
                     const char *sym_spec, int do_verbose) {
    struct stat st;
    if (lstat(path, &st) < 0) {
        fprintf(stderr, "chmod: cannot access '%s': %s\n", path, strerror(errno));
        return 1;
    }

    mode_t old_mode = st.st_mode & 07777;
    mode_t new_mode;

    if (is_octal) {
        new_mode = octal_mode;
    } else {
        new_mode = parse_symbolic(sym_spec, old_mode);
        if (new_mode == (mode_t)-1) {
            fprintf(stderr, "chmod: invalid mode: '%s'\n", sym_spec);
            return 1;
        }
    }

    if (chmod(path, new_mode) < 0) {
        fprintf(stderr, "chmod: changing permissions of '%s': %s\n",
                path, strerror(errno));
        return 1;
    }

    if (do_verbose)
        printf("mode of '%s' changed from %04o to %04o\n",
               path, (unsigned)old_mode, (unsigned)new_mode);

    return 0;
}

/* ── Recursive walk ───────────────────────────────────────────────────── */

static int chmod_recurse(const char *path, int is_octal, mode_t octal_mode,
                          const char *sym_spec, int do_verbose) {
    int ret = chmod_one(path, is_octal, octal_mode, sym_spec, do_verbose);

    struct stat st;
    if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                    continue;
                char child[PATH_MAX];
                snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
                if (chmod_recurse(child, is_octal, octal_mode, sym_spec, do_verbose))
                    ret = 1;
            }
            closedir(d);
        }
    }
    return ret;
}

/* ── Argtable ─────────────────────────────────────────────────────────── */

static void build_chmod_argtable(arg_lit_t  **help,
                                  arg_lit_t  **help_json,
                                  arg_lit_t  **recursive,
                                  arg_lit_t  **verbose,
                                  arg_file_t **args,
                                  arg_end_t  **end,
                                  void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json", "print machine-readable help as JSON");
    *recursive = arg_lit0("R", "recursive", "change permissions recursively");
    *verbose   = arg_lit0("v", "verbose",   "output a diagnostic for every file processed");
    *args      = arg_filen(NULL, NULL, "MODE FILE...", 2, 65,
                           "octal or symbolic mode, followed by file(s)");
    *end       = arg_end(20);

    static void *argtable[7];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *recursive;
    argtable[3] = *verbose;
    argtable[4] = *args;
    argtable[5] = *end;
    argtable[6] = NULL;
    *argtable_out = argtable;
}

void chmod_print_usage(FILE *out);
extern cmd_spec_t cmd_chmod_spec;

/* ── chmod_run ────────────────────────────────────────────────────────── */

int chmod_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json, *recursive, *verbose;
    arg_file_t *args;
    arg_end_t  *end;
    void      **argtable;

    build_chmod_argtable(&help, &help_json, &recursive, &verbose,
                         &args, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 6);
        chmod_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_chmod_spec, argtable, stdout);
        arg_freetable(argtable, 6);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "chmod");
        arg_freetable(argtable, 6);
        chmod_print_usage(stdout);
        return 1;
    }

    int do_recursive = (recursive->count > 0);
    int do_verbose   = (verbose->count > 0);

    /* First positional arg is MODE; rest are the files to chmod */
    const char *mode_str = args->filename[0];
    int nfiles = args->count - 1;
    const char *file_arr[64];
    for (int i = 0; i < nfiles && i < 64; i++)
        file_arr[i] = args->filename[i + 1];

    mode_t octal_mode = 0;
    int is_octal = 0;
    if (parse_mode_str(mode_str, &octal_mode, &is_octal) < 0) {
        fprintf(stderr, "chmod: invalid mode: '%s'\n", mode_str);
        arg_freetable(argtable, 6);
        return 1;
    }

    arg_freetable(argtable, 6);

    if (nfiles == 0) {
        fprintf(stderr, "chmod: missing file operand\n");
        return 1;
    }

    int ret = 0;
    for (int i = 0; i < nfiles; i++) {
        int r = do_recursive
            ? chmod_recurse(file_arr[i], is_octal, octal_mode, mode_str, do_verbose)
            : chmod_one(file_arr[i], is_octal, octal_mode, mode_str, do_verbose);
        if (r) ret = 1;
    }
    return ret;
}

/* ── chmod_print_usage ────────────────────────────────────────────────── */

void chmod_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json, *recursive, *verbose;
    arg_file_t *args;
    arg_end_t  *end;
    void      **argtable;

    build_chmod_argtable(&help, &help_json, &recursive, &verbose,
                         &args, &end, &argtable);
    fprintf(out, "Usage: chmod ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nChange file mode bits.\n");
    fprintf(out, "\n  MODE: octal (755, 644) or symbolic ([ugoa][+-=][rwxXst]+)\n");
    fprintf(out, "  Symbolic examples: u+x  go-w  a=r  u+x,g-w  o=\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-28s %s\n");
    arg_freetable(argtable, 6);
}

cmd_spec_t cmd_chmod_spec = {
    .name      = "chmod",
    .summary   = "change file mode bits",
    .long_help = "Change the permission bits of each FILE. "
                 "MODE can be octal (e.g. 755) or symbolic (e.g. u+x, go-w, a=r). "
                 "Symbolic clauses: [ugoa][+-=][rwxXst]+, comma-separated. "
                 "Use -R to recurse into directories, -v for verbose output.",
    .run         = chmod_run,
    .print_usage = chmod_print_usage,
};

void register_chmod_command(void) {
    register_command(&cmd_chmod_spec);
}
