#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"
#include "hash_utils.h"

/* ── argtable ─────────────────────────────────────────────────────────── */

static void build_sha256sum_argtable(arg_lit_t  **help,
                                      arg_lit_t  **help_json,
                                      arg_lit_t  **check,
                                      arg_lit_t  **binary,
                                      arg_file_t **files,
                                      arg_end_t  **end,
                                      void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json", "print machine-readable help as JSON");
    *check     = arg_lit0("c", "check",     "read checksums from FILE and verify");
    *binary    = arg_lit0("b", "binary",    "read in binary mode (no-op on Linux)");
    *files     = arg_filen(NULL, NULL, "FILE", 0, 64, "files to hash; stdin if none");
    *end       = arg_end(20);

    static void *argtable[7];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *check;
    argtable[3] = *binary;
    argtable[4] = *files;
    argtable[5] = *end;
    argtable[6] = NULL;
    *argtable_out = argtable;
}

void sha256sum_print_usage(FILE *out);
extern cmd_spec_t cmd_sha256sum_spec;

/* ── check helper ─────────────────────────────────────────────────────── */

static int sha256_check_stream(FILE *cf)
{
    int fails = 0, ok = 0;
    char line[600];
    while (fgets(line, sizeof(line), cf)) {
        size_t len = strlen(line);
        if (len < 67) continue; /* 64 hash + ' ' + ' '/'*' + at least 1 char */
        char expected[65];
        memcpy(expected, line, 64);
        expected[64] = '\0';
        int valid = 1;
        for (int j = 0; j < 64; j++) {
            char ch = expected[j];
            if (!((ch>='0'&&ch<='9')||(ch>='a'&&ch<='f')||(ch>='A'&&ch<='F')))
                { valid = 0; break; }
        }
        if (!valid || line[64] != ' ') continue;
        char *fname = line + 65;
        if      (*fname == ' ') fname++;
        else if (*fname == '*') fname++;
        fname[strcspn(fname, "\r\n")] = '\0';
        if (!*fname) continue;

        FILE *f = fopen(fname, "rb");
        if (!f) {
            fprintf(stderr, "sha256sum: %s: No such file or directory\n", fname);
            fails++;
            continue;
        }
        const char *got = sha256_hexdigest(f);
        fclose(f);
        if (strcmp(got, expected) == 0) { printf("%s: OK\n", fname); ok++; }
        else                            { printf("%s: FAILED\n", fname); fails++; }
    }
    if (fails > 0)
        fprintf(stderr, "sha256sum: WARNING: %d computed checksum%s did NOT match\n",
                fails, fails == 1 ? "" : "s");
    (void)ok;
    return (fails > 0) ? 1 : 0;
}

/* ── sha256sum_run ─────────────────────────────────────────────────────── */

int sha256sum_run(int argc, char **argv)
{
    arg_lit_t  *help, *help_json, *check, *binary;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_sha256sum_argtable(&help, &help_json, &check, &binary, &files, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 6);
        sha256sum_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_sha256sum_spec, argtable, stdout);
        arg_freetable(argtable, 6);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "sha256sum");
        arg_freetable(argtable, 6);
        sha256sum_print_usage(stdout);
        return 1;
    }

    int do_check = (check->count > 0);
    (void)binary;

    int nfiles = files->count;
    const char *paths[64];
    for (int i = 0; i < nfiles && i < 64; i++)
        paths[i] = files->filename[i];
    arg_freetable(argtable, 6);

    if (do_check) {
        if (nfiles == 0) return sha256_check_stream(stdin);
        int rc = 0;
        for (int i = 0; i < nfiles; i++) {
            FILE *cf = strcmp(paths[i], "-") == 0
                       ? stdin : fopen(paths[i], "r");
            if (!cf) {
                fprintf(stderr, "sha256sum: %s: %s\n", paths[i], strerror(errno));
                rc = 1; continue;
            }
            if (sha256_check_stream(cf)) rc = 1;
            if (cf != stdin) fclose(cf);
        }
        return rc;
    }

    if (nfiles == 0) {
        printf("%s  -\n", sha256_hexdigest(stdin));
        return 0;
    }
    int rc = 0;
    for (int i = 0; i < nfiles; i++) {
        FILE *f = strcmp(paths[i], "-") == 0
                  ? stdin : fopen(paths[i], "rb");
        if (!f) {
            fprintf(stderr, "sha256sum: %s: %s\n", paths[i], strerror(errno));
            rc = 1; continue;
        }
        const char *h = sha256_hexdigest(f);
        if (f != stdin) fclose(f);
        printf("%s  %s\n", h, paths[i]);
    }
    return rc;
}

/* ── sha256sum_print_usage ─────────────────────────────────────────────── */

void sha256sum_print_usage(FILE *out)
{
    arg_lit_t  *help, *help_json, *check, *binary;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_sha256sum_argtable(&help, &help_json, &check, &binary, &files, &end, &argtable);
    fprintf(out, "Usage: sha256sum ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nCompute or verify SHA-256 (256-bit) message digests.\n");
    fprintf(out, "Output format:  <hash>  <filename>\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-28s %s\n");
    arg_freetable(argtable, 6);
}

cmd_spec_t cmd_sha256sum_spec = {
    .name      = "sha256sum",
    .summary   = "compute and check SHA-256 message digests",
    .long_help = "Print or check SHA-256 (256-bit) checksums. "
                 "With no FILE, or when FILE is -, read standard input. "
                 "Use -c to verify checksums read from a checksum file. "
                 "Output format: <64-hex-chars>  <filename>.",
    .run         = sha256sum_run,
    .print_usage = sha256sum_print_usage,
};

void register_sha256sum_command(void) {
    register_command(&cmd_sha256sum_spec);
}
