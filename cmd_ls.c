#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

static void build_ls_argtable(arg_lit_t  **help,
                               arg_lit_t  **help_json,
                               arg_lit_t  **all,
                               arg_lit_t  **lng,
                               arg_file_t **files,
                               arg_end_t  **end,
                               void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json", "print machine-readable help as JSON");
    *all       = arg_lit0("a", "all",        "do not ignore entries starting with .");
    *lng       = arg_lit0("l", NULL,         "use a long listing format");
    *files     = arg_filen(NULL, NULL, "[DIR...]", 0, 32, "directories to list (default: .)");
    *end       = arg_end(20);

    static void *argtable[7];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *all;
    argtable[3] = *lng;
    argtable[4] = *files;
    argtable[5] = *end;
    argtable[6] = NULL;

    *argtable_out = argtable;
}

void ls_print_usage(FILE *out);
extern cmd_spec_t cmd_ls_spec;

int ls_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json, *all, *lng;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_ls_argtable(&help, &help_json, &all, &lng, &files, &end, &argtable);

    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 6);
        ls_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_ls_spec, argtable, stdout);
        arg_freetable(argtable, 6);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "ls");
        arg_freetable(argtable, 6);
        ls_print_usage(stdout);
        return 1;
    }

    int dir_count = files->count;
    if (dir_count == 0) {
        files->filename[0] = ".";
        files->count = 1;
        dir_count = 1;
    }

    for (int i = 0; i < dir_count; i++) {
        const char *dirpath = files->filename[i];
        DIR *dir = opendir(dirpath);

        if (dir == NULL) {
            fprintf(stderr, "ls: cannot open '%s': %s\n",
                    dirpath, strerror(errno));
            continue;
        }

        if (dir_count > 1)
            printf("\n%s:\n", dirpath);

        if (lng->count > 0) {
            printf("%-10s %-8s %-8s %8s %-20s %s\n",
                   "Perms", "Owner", "Group", "Size", "Modified", "Name");
            printf("-------------------------------------------------------\n");
        }

        struct dirent *entry;
        errno = 0;
        while ((entry = readdir(dir)) != NULL) {
            if (all->count == 0 && entry->d_name[0] == '.')
                continue;

            if (lng->count > 0) {
                char fullpath[1024];
                snprintf(fullpath, sizeof(fullpath),
                         "%s/%s", dirpath, entry->d_name);

                struct stat st;
                if (stat(fullpath, &st) != 0) {
                    fprintf(stderr, "ls: cannot stat '%s': %s\n",
                            fullpath, strerror(errno));
                    continue;
                }

                char perms[11];
                perms[0] = S_ISDIR(st.st_mode) ? 'd' :
                           S_ISLNK(st.st_mode) ? 'l' : '-';
                perms[1] = (st.st_mode & S_IRUSR) ? 'r' : '-';
                perms[2] = (st.st_mode & S_IWUSR) ? 'w' : '-';
                perms[3] = (st.st_mode & S_IXUSR) ? 'x' : '-';
                perms[4] = (st.st_mode & S_IRGRP) ? 'r' : '-';
                perms[5] = (st.st_mode & S_IWGRP) ? 'w' : '-';
                perms[6] = (st.st_mode & S_IXGRP) ? 'x' : '-';
                perms[7] = (st.st_mode & S_IROTH) ? 'r' : '-';
                perms[8] = (st.st_mode & S_IWOTH) ? 'w' : '-';
                perms[9] = (st.st_mode & S_IXOTH) ? 'x' : '-';
                perms[10] = '\0';

                struct passwd *pw = getpwuid(st.st_uid);
                struct group  *gr = getgrgid(st.st_gid);
                char owner[32], group[32];
                snprintf(owner, sizeof(owner), "%s",
                         pw ? pw->pw_name : "unknown");
                snprintf(group, sizeof(group), "%s",
                         gr ? gr->gr_name : "unknown");

                char tbuf[32];
                struct tm *tm = localtime(&st.st_mtime);
                strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm);

                printf("%-10s %-8s %-8s %8ld %-20s %s\n",
                       perms, owner, group,
                       (long)st.st_size, tbuf, entry->d_name);
            } else {
                printf("%s\n", entry->d_name);
            }
        }
        if (errno != 0) {
            fprintf(stderr, "ls: error reading '%s': %s\n",
                    dirpath, strerror(errno));
        }
        closedir(dir);
    }

    arg_freetable(argtable, 6);
    return 0;
}

void ls_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json, *all, *lng;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_ls_argtable(&help, &help_json, &all, &lng, &files, &end, &argtable);

    fprintf(out, "Usage: ls ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-20s %s\n");

    arg_freetable(argtable, 6);
}

cmd_spec_t cmd_ls_spec = {
    .name        = "ls",
    .summary     = "list directory contents",
    .long_help   = "List information about files "
                   "(the current directory by default).",
    .run         = ls_run,
    .print_usage = ls_print_usage,
};

void register_ls_command(void) {
    register_command(&cmd_ls_spec);
}
