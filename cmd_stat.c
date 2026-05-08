#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

static void build_stat_argtable(arg_lit_t  **help,
                                 arg_lit_t  **help_json,
                                 arg_lit_t  **compact,
                                 arg_str_t  **fmt,
                                 arg_file_t **files,
                                 arg_end_t  **end,
                                 void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json", "print machine-readable help as JSON");
    *compact   = arg_lit0("c", "compact",   "use compact terse output");
    *fmt       = arg_str0("f", "format",    "FMT", "use specified format string");
    *files     = arg_filen(NULL, NULL, "FILE...", 1, 64, "files to stat");
    *end       = arg_end(20);

    static void *argtable[7];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *compact;
    argtable[3] = *fmt;
    argtable[4] = *files;
    argtable[5] = *end;
    argtable[6] = NULL;

    *argtable_out = argtable;
}

void stat_print_usage(FILE *out);
extern cmd_spec_t cmd_stat_spec;

static void print_stat_default(const char *filepath, int column_mode) {
    struct stat st;
    if (stat(filepath, &st) != 0) {
        fprintf(stderr, "stat: cannot stat '%s': %s\n",
                filepath, strerror(errno));
        return;
    }

    char perms[11];
    perms[0] = S_ISDIR(st.st_mode)  ? 'd' :
               S_ISLNK(st.st_mode)  ? 'l' :
               S_ISBLK(st.st_mode)  ? 'b' :
               S_ISCHR(st.st_mode)  ? 'c' :
               S_ISFIFO(st.st_mode) ? 'p' :
               S_ISSOCK(st.st_mode) ? 's' : '-';
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

    if (column_mode) {
        printf("%s %10ld %s\n", perms, (long)st.st_size, filepath);
        return;
    }

    printf("  File: %s\n", filepath);
    printf("  Size: %-10ld\tBlocks: %-10ld\t",
           (long)st.st_size, (long)st.st_blocks);

    if (S_ISREG(st.st_mode))       printf("regular file\n");
    else if (S_ISDIR(st.st_mode))  printf("directory\n");
    else if (S_ISLNK(st.st_mode))  printf("symbolic link\n");
    else if (S_ISCHR(st.st_mode))  printf("character device\n");
    else if (S_ISBLK(st.st_mode))  printf("block device\n");
    else if (S_ISFIFO(st.st_mode)) printf("FIFO/pipe\n");
    else if (S_ISSOCK(st.st_mode)) printf("socket\n");
    else                           printf("unknown\n");

    printf("  Device: %lxh/%lud\tInode: %-10lu\tLinks: %lu\n",
           (unsigned long)st.st_dev, (unsigned long)st.st_dev,
           (unsigned long)st.st_ino, (unsigned long)st.st_nlink);

    printf("  Access: (%04o/%s)\tUid: (%d/%s)\tGid: (%d/%s)\n",
           (unsigned int)(st.st_mode & 07777), perms,
           st.st_uid,
           getpwuid(st.st_uid) ? getpwuid(st.st_uid)->pw_name : "unknown",
           st.st_gid,
           getgrgid(st.st_gid) ? getgrgid(st.st_gid)->gr_name : "unknown");

    char atbuf[32], mtbuf[32], ctbuf[32];
    struct tm *atm = localtime(&st.st_atime);
    struct tm *mtm = localtime(&st.st_mtime);
    struct tm *ctm = localtime(&st.st_ctime);
    strftime(atbuf, sizeof(atbuf), "%Y-%m-%d %H:%M:%S", atm);
    strftime(mtbuf, sizeof(mtbuf), "%Y-%m-%d %H:%M:%S", mtm);
    strftime(ctbuf, sizeof(ctbuf), "%Y-%m-%d %H:%M:%S", ctm);

    printf("  Access: %s\n", atbuf);
    printf("  Modify: %s\n", mtbuf);
    printf("  Change: %s\n\n", ctbuf);
}

static void print_stat_custom(const char *filepath, const char *fmt) {
    struct stat st;
    if (stat(filepath, &st) != 0) {
        fprintf(stderr, "stat: cannot stat '%s': %s\n",
                filepath, strerror(errno));
        return;
    }

    for (const char *p = fmt; *p; p++) {
        if (*p == '%') {
            p++;
            switch (*p) {
            case 'n': printf("%s", filepath); break;
            case 'N': printf("'%s'", filepath); break;
            case 's': printf("%ld", (long)st.st_size); break;
            case 'b': printf("%ld", (long)st.st_blocks); break;
            case 'i': printf("%lu", (unsigned long)st.st_ino); break;
            case 'l': printf("%lu", (unsigned long)st.st_nlink); break;
            case 'u': printf("%d", st.st_uid); break;
            case 'g': printf("%d", st.st_gid); break;
            case 'U': {
                struct passwd *pw = getpwuid(st.st_uid);
                printf("%s", pw ? pw->pw_name : "unknown");
                break;
            }
            case 'G': {
                struct group *gr = getgrgid(st.st_gid);
                printf("%s", gr ? gr->gr_name : "unknown");
                break;
            }
            case 'p': printf("%o", (unsigned int)(st.st_mode & 07777)); break;
            case 'F':
                printf("%s",
                       S_ISDIR(st.st_mode)  ? "directory" :
                       S_ISLNK(st.st_mode)  ? "symlink"   :
                       S_ISBLK(st.st_mode)  ? "block"     :
                       S_ISCHR(st.st_mode)  ? "chardev"   :
                       S_ISFIFO(st.st_mode) ? "fifo"      :
                       S_ISSOCK(st.st_mode) ? "socket"    : "regular");
                break;
            case 'x': {
                char tbuf[32];
                struct tm *tm = localtime(&st.st_atime);
                strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm);
                printf("%s", tbuf);
                break;
            }
            case 'y': {
                char tbuf[32];
                struct tm *tm = localtime(&st.st_mtime);
                strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm);
                printf("%s", tbuf);
                break;
            }
            case 'z': {
                char tbuf[32];
                struct tm *tm = localtime(&st.st_ctime);
                strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm);
                printf("%s", tbuf);
                break;
            }
            case '%': putchar('%'); break;
            case '\0': return;
            default:   putchar('%'); putchar(*p); break;
            }
        } else {
            putchar(*p);
        }
    }
    putchar('\n');
}

int stat_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json, *compact;
    arg_str_t  *fmt;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_stat_argtable(&help, &help_json, &compact, &fmt, &files, &end, &argtable);

    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 6);
        stat_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_stat_spec, argtable, stdout);
        arg_freetable(argtable, 6);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "stat");
        arg_freetable(argtable, 6);
        stat_print_usage(stdout);
        return 1;
    }

    int         column_mode = (compact->count > 0);
    const char *custom_fmt  = (fmt->count > 0) ? fmt->sval[0] : NULL;

    for (int i = 0; i < files->count; i++) {
        if (custom_fmt)
            print_stat_custom(files->filename[i], custom_fmt);
        else
            print_stat_default(files->filename[i], column_mode);
    }

    arg_freetable(argtable, 6);
    return 0;
}

void stat_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json, *compact;
    arg_str_t  *fmt;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_stat_argtable(&help, &help_json, &compact, &fmt, &files, &end, &argtable);

    fprintf(out, "Usage: stat ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-20s %s\n");

    arg_freetable(argtable, 6);
}

cmd_spec_t cmd_stat_spec = {
    .name        = "stat",
    .summary     = "display file or file system status",
    .long_help   = "Display detailed information about each FILE.",
    .run         = stat_run,
    .print_usage = stat_print_usage,
};

void register_stat_command(void) {
    register_command(&cmd_stat_spec);
}
