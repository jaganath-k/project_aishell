#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <pwd.h>
#include <grp.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

/* ── OWNER[:GROUP] parser ─────────────────────────────────────────────── */

static int parse_owner_spec(const char *spec, uid_t *uid_out, gid_t *gid_out) {
    char buf[256];
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    uid_t uid = (uid_t)-1;
    gid_t gid = (gid_t)-1;

    char *colon = strchr(buf, ':');
    if (colon) {
        *colon = '\0';
        const char *grp_str = colon + 1;

        /* Parse owner (may be empty when spec is ":group") */
        if (*buf) {
            struct passwd *pw = getpwnam(buf);
            if (pw) {
                uid = pw->pw_uid;
            } else {
                char *end;
                long n = strtol(buf, &end, 10);
                if (*end == '\0') uid = (uid_t)n;
                else {
                    fprintf(stderr, "chown: invalid user: '%s'\n", buf);
                    return -1;
                }
            }
        }

        /* Parse group (may be empty when spec is "owner:") */
        if (*grp_str) {
            struct group *gr = getgrnam(grp_str);
            if (gr) {
                gid = gr->gr_gid;
            } else {
                char *end;
                long n = strtol(grp_str, &end, 10);
                if (*end == '\0') gid = (gid_t)n;
                else {
                    fprintf(stderr, "chown: invalid group: '%s'\n", grp_str);
                    return -1;
                }
            }
        }
    } else {
        /* No colon: only owner */
        struct passwd *pw = getpwnam(buf);
        if (pw) {
            uid = pw->pw_uid;
        } else {
            char *end;
            long n = strtol(buf, &end, 10);
            if (*end == '\0') uid = (uid_t)n;
            else {
                fprintf(stderr, "chown: invalid user: '%s'\n", buf);
                return -1;
            }
        }
    }

    *uid_out = uid;
    *gid_out = gid;
    return 0;
}

/* ── Apply lchown to one path ─────────────────────────────────────────── */

static int chown_one(const char *path, uid_t uid, gid_t gid,
                     int do_verbose, const char *spec) {
    if (lchown(path, uid, gid) < 0) {
        fprintf(stderr, "chown: changing ownership of '%s': %s\n",
                path, strerror(errno));
        return 1;
    }
    if (do_verbose)
        printf("changed ownership of '%s' to %s\n", path, spec);
    return 0;
}

/* ── Recursive walk ───────────────────────────────────────────────────── */

static int chown_recurse(const char *path, uid_t uid, gid_t gid,
                          int do_verbose, const char *spec) {
    int ret = chown_one(path, uid, gid, do_verbose, spec);

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
                if (chown_recurse(child, uid, gid, do_verbose, spec))
                    ret = 1;
            }
            closedir(d);
        }
    }
    return ret;
}

/* ── Argtable ─────────────────────────────────────────────────────────── */

static void build_chown_argtable(arg_lit_t  **help,
                                  arg_lit_t  **help_json,
                                  arg_lit_t  **recursive,
                                  arg_lit_t  **verbose,
                                  arg_file_t **args,
                                  arg_end_t  **end,
                                  void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json", "print machine-readable help as JSON");
    *recursive = arg_lit0("R", "recursive", "change ownership recursively");
    *verbose   = arg_lit0("v", "verbose",   "print a diagnostic for each file changed");
    *args      = arg_filen(NULL, NULL, "OWNER[:GROUP] FILE...", 2, 65,
                           "owner spec followed by file(s)");
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

void chown_print_usage(FILE *out);
extern cmd_spec_t cmd_chown_spec;

/* ── chown_run ────────────────────────────────────────────────────────── */

int chown_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json, *recursive, *verbose;
    arg_file_t *args;
    arg_end_t  *end;
    void      **argtable;

    build_chown_argtable(&help, &help_json, &recursive, &verbose,
                         &args, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 6);
        chown_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_chown_spec, argtable, stdout);
        arg_freetable(argtable, 6);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "chown");
        arg_freetable(argtable, 6);
        chown_print_usage(stdout);
        return 1;
    }

    int do_recursive = (recursive->count > 0);
    int do_verbose   = (verbose->count > 0);

    /* First positional is OWNER[:GROUP], rest are files */
    const char *spec   = args->filename[0];
    int nfiles         = args->count - 1;
    const char *file_arr[64];
    for (int i = 0; i < nfiles && i < 64; i++)
        file_arr[i] = args->filename[i + 1];

    uid_t uid;
    gid_t gid;
    if (parse_owner_spec(spec, &uid, &gid) < 0) {
        arg_freetable(argtable, 6);
        return 1;
    }

    arg_freetable(argtable, 6);

    if (nfiles == 0) {
        fprintf(stderr, "chown: missing file operand\n");
        return 1;
    }

    int ret = 0;
    for (int i = 0; i < nfiles; i++) {
        int r = do_recursive
            ? chown_recurse(file_arr[i], uid, gid, do_verbose, spec)
            : chown_one(file_arr[i], uid, gid, do_verbose, spec);
        if (r) ret = 1;
    }
    return ret;
}

/* ── chown_print_usage ────────────────────────────────────────────────── */

void chown_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json, *recursive, *verbose;
    arg_file_t *args;
    arg_end_t  *end;
    void      **argtable;

    build_chown_argtable(&help, &help_json, &recursive, &verbose,
                         &args, &end, &argtable);
    fprintf(out, "Usage: chown ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nChange file owner and group.\n");
    fprintf(out, "\n  OWNER[:GROUP] formats:\n");
    fprintf(out, "    owner        — change owner only\n");
    fprintf(out, "    owner:group  — change owner and group\n");
    fprintf(out, "    :group       — change group only\n");
    fprintf(out, "  Names or numeric IDs are accepted.\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-28s %s\n");
    arg_freetable(argtable, 6);
}

cmd_spec_t cmd_chown_spec = {
    .name      = "chown",
    .summary   = "change file owner and group",
    .long_help = "Change the owner and/or group of each FILE using lchown(2). "
                 "OWNER[:GROUP] accepts names or numeric IDs. "
                 "Omit OWNER (use :GROUP) to change only the group. "
                 "Use -R to recurse into directories, -v for verbose output.",
    .run         = chown_run,
    .print_usage = chown_print_usage,
};

void register_chown_command(void) {
    register_command(&cmd_chown_spec);
}
