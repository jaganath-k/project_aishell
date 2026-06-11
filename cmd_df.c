#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <mntent.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

/* ── Formatters ───────────────────────────────────────────────────────── */

static void fmt_df_size(unsigned long long bytes, int human, char *buf, size_t bsz) {
    if (!human) {
        snprintf(buf, bsz, "%llu", (bytes + 1023ULL) / 1024ULL);
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

/* ── Pseudo-filesystem filter ─────────────────────────────────────────── */

static int is_pseudo(const char *fstype) {
    static const char *skip[] = {
        "proc", "sysfs", "devtmpfs", "devpts", "cgroup", "cgroup2",
        "securityfs", "debugfs", "pstore", "configfs", "tracefs",
        "bpf", "fusectl", "mqueue", "hugetlbfs", "binfmt_misc",
        "autofs", "nsfs", "efivarfs", NULL
    };
    for (int i = 0; skip[i]; i++)
        if (strcmp(fstype, skip[i]) == 0) return 1;
    return 0;
}

/* ── Argtable ─────────────────────────────────────────────────────────── */

static void build_df_argtable(arg_lit_t  **help,
                               arg_lit_t  **help_json,
                               arg_lit_t  **human,
                               arg_lit_t  **print_type,
                               arg_file_t **paths,
                               arg_end_t  **end,
                               void       ***argtable_out)
{
    *help       = arg_lit0(NULL, "help",         "show help and exit");
    *help_json  = arg_lit0(NULL, "help-json",     "print machine-readable help as JSON");
    *human      = arg_lit0("h", "human-readable","print sizes in human-readable form (1.5K, 3.2M)");
    *print_type = arg_lit0("T", "print-type",    "include filesystem type column in output");
    *paths      = arg_filen(NULL, NULL, "PATH", 0, 64,
                            "show only the filesystem containing PATH");
    *end        = arg_end(20);

    static void *argtable[7];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *human;
    argtable[3] = *print_type;
    argtable[4] = *paths;
    argtable[5] = *end;
    argtable[6] = NULL;
    *argtable_out = argtable;
}

void df_print_usage(FILE *out);
extern cmd_spec_t cmd_df_spec;

/* ── df_run ───────────────────────────────────────────────────────────── */

int df_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json, *human, *print_type;
    arg_file_t *paths;
    arg_end_t  *end;
    void      **argtable;

    build_df_argtable(&help, &help_json, &human, &print_type, &paths, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 6);
        df_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_df_spec, argtable, stdout);
        arg_freetable(argtable, 6);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "df");
        arg_freetable(argtable, 6);
        df_print_usage(stdout);
        return 1;
    }

    int do_human = (human->count > 0);
    int do_type  = (print_type->count > 0);

    /* Build devno filter from given paths (pointers into argv — safe after free) */
    int npaths = paths->count;
    const char *path_arr[64];
    for (int i = 0; i < npaths; i++)
        path_arr[i] = paths->filename[i];
    arg_freetable(argtable, 6);

    dev_t filter_devs[64];
    int nfilter = 0;
    for (int i = 0; i < npaths; i++) {
        struct stat st;
        if (stat(path_arr[i], &st) < 0) {
            fprintf(stderr, "df: %s: %s\n", path_arr[i], strerror(errno));
            return 1;
        }
        filter_devs[nfilter++] = st.st_dev;
    }

    /* Print header */
    if (do_type)
        printf("%-20s %-10s %7s %7s %7s %5s %s\n",
               "Filesystem", "Type", "Size", "Used", "Avail", "Use%", "Mounted on");
    else
        printf("%-20s %7s %7s %7s %5s %s\n",
               "Filesystem", "Size", "Used", "Avail", "Use%", "Mounted on");

    /* Iterate /proc/mounts */
    FILE *mf = setmntent("/proc/mounts", "r");
    if (!mf) {
        fprintf(stderr, "df: cannot open /proc/mounts: %s\n", strerror(errno));
        return 1;
    }

    struct mntent *ent;
    while ((ent = getmntent(mf)) != NULL) {
        /* Without PATH filter: skip pseudo-filesystems */
        if (nfilter == 0 && is_pseudo(ent->mnt_type))
            continue;

        /* Get the device number of this mount point */
        struct stat mst;
        if (stat(ent->mnt_dir, &mst) < 0)
            continue;

        /* With PATH filter: only show filesystems containing given paths */
        if (nfilter > 0) {
            int match = 0;
            for (int i = 0; i < nfilter; i++)
                if (filter_devs[i] == mst.st_dev) { match = 1; break; }
            if (!match) continue;
        }

        /* Get filesystem statistics */
        struct statvfs svfs;
        if (statvfs(ent->mnt_dir, &svfs) < 0)
            continue;
        if (svfs.f_blocks == 0 || svfs.f_frsize == 0)
            continue;

        unsigned long long total = (unsigned long long)svfs.f_blocks * svfs.f_frsize;
        unsigned long long free_b = (unsigned long long)svfs.f_bfree  * svfs.f_frsize;
        unsigned long long avail_b = (unsigned long long)svfs.f_bavail * svfs.f_frsize;
        unsigned long long used_b = total - free_b;
        int pct = (int)((used_b * 100ULL) / total);

        char sz[16], us[16], av[16], pctbuf[8];
        fmt_df_size(total,   do_human, sz, sizeof(sz));
        fmt_df_size(used_b,  do_human, us, sizeof(us));
        fmt_df_size(avail_b, do_human, av, sizeof(av));
        snprintf(pctbuf, sizeof(pctbuf), "%d%%", pct);

        if (do_type)
            printf("%-20s %-10s %7s %7s %7s %5s %s\n",
                   ent->mnt_fsname, ent->mnt_type, sz, us, av, pctbuf, ent->mnt_dir);
        else
            printf("%-20s %7s %7s %7s %5s %s\n",
                   ent->mnt_fsname, sz, us, av, pctbuf, ent->mnt_dir);
    }

    endmntent(mf);
    return 0;
}

/* ── df_print_usage ───────────────────────────────────────────────────── */

void df_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json, *human, *print_type;
    arg_file_t *paths;
    arg_end_t  *end;
    void      **argtable;

    build_df_argtable(&help, &help_json, &human, &print_type, &paths, &end, &argtable);
    fprintf(out, "Usage: df ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nReport filesystem disk space usage.\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-32s %s\n");
    arg_freetable(argtable, 6);
}

cmd_spec_t cmd_df_spec = {
    .name      = "df",
    .summary   = "report filesystem disk space usage",
    .long_help = "Display available disk space for each filesystem. "
                 "Reads /proc/mounts and calls statvfs() for each entry. "
                 "Use -h for human-readable sizes, -T to show filesystem type. "
                 "Pass PATH arguments to show only the filesystem(s) containing those paths.",
    .run         = df_run,
    .print_usage = df_print_usage,
};

void register_df_command(void) {
    register_command(&cmd_df_spec);
}
