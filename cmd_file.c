#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

/* ── Magic-byte / content detection ──────────────────────────────────── */

static void detect_regular(const char *path, char *desc, size_t desz) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(desc, desz, "cannot open: %s", strerror(errno));
        return;
    }

    unsigned char buf[264];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    if (n == 0) {
        snprintf(desc, desz, "empty");
        return;
    }

    /* ELF */
    if (n >= 4 && buf[0] == 0x7f && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F') {
        const char *bits = (n >= 5 && buf[4] == 2) ? "64-bit" : "32-bit";
        snprintf(desc, desz, "ELF %s executable", bits);
        return;
    }
    /* PNG */
    if (n >= 4 && buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' && buf[3] == 'G') {
        snprintf(desc, desz, "PNG image data");
        return;
    }
    /* JPEG */
    if (n >= 3 && buf[0] == 0xff && buf[1] == 0xd8 && buf[2] == 0xff) {
        snprintf(desc, desz, "JPEG image data");
        return;
    }
    /* PDF */
    if (n >= 4 && buf[0] == '%' && buf[1] == 'P' && buf[2] == 'D' && buf[3] == 'F') {
        snprintf(desc, desz, "PDF document");
        return;
    }
    /* ZIP / JAR / DOCX */
    if (n >= 4 && buf[0] == 'P' && buf[1] == 'K' && buf[2] == 0x03 && buf[3] == 0x04) {
        snprintf(desc, desz, "Zip archive data");
        return;
    }
    /* gzip */
    if (n >= 2 && buf[0] == 0x1f && buf[1] == 0x8b) {
        snprintf(desc, desz, "gzip compressed data");
        return;
    }
    /* tar (ustar magic at offset 257) */
    if (n >= 262 && memcmp(buf + 257, "ustar", 5) == 0) {
        snprintf(desc, desz, "POSIX tar archive");
        return;
    }
    /* Shell script / shebang */
    if (n >= 2 && buf[0] == '#' && buf[1] == '!') {
        /* Read first line to extract interpreter */
        char interp[128] = "";
        size_t i = 2;
        while (i < n && buf[i] == ' ') i++;   /* skip spaces after #! */
        size_t j = 0;
        while (i < n && buf[i] != '\n' && buf[i] != '\r' && j < sizeof(interp)-1)
            interp[j++] = (char)buf[i++];
        interp[j] = '\0';
        snprintf(desc, desz, "shell script, interpreter: %s", interp);
        return;
    }
    /* UTF-8 / ASCII text: all bytes are printable or common whitespace */
    int is_text = 1;
    for (size_t i = 0; i < n && i < 16; i++) {
        unsigned char c = buf[i];
        if (!((c >= 0x09 && c <= 0x0d) || (c >= 0x20 && c <= 0x7e) || c >= 0x80)) {
            is_text = 0;
            break;
        }
    }
    if (is_text)
        snprintf(desc, desz, "ASCII text");
    else
        snprintf(desc, desz, "binary data");
}

static void describe_path(const char *path, int brief) {
    struct stat st;
    if (lstat(path, &st) < 0) {
        fprintf(stderr, "file: %s: %s\n", path, strerror(errno));
        return;
    }

    char desc[256];

    if (S_ISLNK(st.st_mode)) {
        char target[232] = "";
        ssize_t len = readlink(path, target, sizeof(target) - 1);
        if (len >= 0) target[len] = '\0';
        snprintf(desc, sizeof(desc), "symbolic link to %s", target);
    } else if (S_ISDIR(st.st_mode)) {
        snprintf(desc, sizeof(desc), "directory");
    } else if (S_ISBLK(st.st_mode)) {
        snprintf(desc, sizeof(desc), "block special file");
    } else if (S_ISCHR(st.st_mode)) {
        snprintf(desc, sizeof(desc), "character special file");
    } else if (S_ISFIFO(st.st_mode)) {
        snprintf(desc, sizeof(desc), "fifo (named pipe)");
    } else if (S_ISSOCK(st.st_mode)) {
        snprintf(desc, sizeof(desc), "socket");
    } else if (S_ISREG(st.st_mode)) {
        detect_regular(path, desc, sizeof(desc));
    } else {
        snprintf(desc, sizeof(desc), "unknown type");
    }

    if (brief)
        printf("%s\n", desc);
    else
        printf("%s: %s\n", path, desc);
}

/* ── Argtable ─────────────────────────────────────────────────────────── */

static void build_file_argtable(arg_lit_t  **help,
                                 arg_lit_t  **help_json,
                                 arg_lit_t  **brief,
                                 arg_file_t **files,
                                 arg_end_t  **end,
                                 void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json", "print machine-readable help as JSON");
    *brief     = arg_lit0("b", "brief",     "do not prepend filename to output");
    *files     = arg_filen(NULL, NULL, "FILE", 1, 64, "file(s) to inspect");
    *end       = arg_end(20);

    static void *argtable[6];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *brief;
    argtable[3] = *files;
    argtable[4] = *end;
    argtable[5] = NULL;
    *argtable_out = argtable;
}

void file_print_usage(FILE *out);
extern cmd_spec_t cmd_file_spec;

/* ── file_run ─────────────────────────────────────────────────────────── */

int file_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json, *brief;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_file_argtable(&help, &help_json, &brief, &files, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 5);
        file_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_file_spec, argtable, stdout);
        arg_freetable(argtable, 5);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "file");
        arg_freetable(argtable, 5);
        file_print_usage(stdout);
        return 1;
    }

    int do_brief = (brief->count > 0);
    int nfiles   = files->count;
    const char *paths[64];
    for (int i = 0; i < nfiles && i < 64; i++)
        paths[i] = files->filename[i];

    arg_freetable(argtable, 5);

    for (int i = 0; i < nfiles; i++)
        describe_path(paths[i], do_brief);

    return 0;
}

/* ── file_print_usage ─────────────────────────────────────────────────── */

void file_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json, *brief;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_file_argtable(&help, &help_json, &brief, &files, &end, &argtable);
    fprintf(out, "Usage: file ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nDetermine file type by examining content (no libmagic).\n");
    fprintf(out, "\n  Detects: ELF, PNG, JPEG, PDF, ZIP, gzip, tar,\n");
    fprintf(out, "           shell scripts (#!), ASCII text, binary data.\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-28s %s\n");
    arg_freetable(argtable, 5);
}

cmd_spec_t cmd_file_spec = {
    .name      = "file",
    .summary   = "determine file type",
    .long_help = "Determine the type of each FILE by examining its content. "
                 "Detects ELF executables, PNG/JPEG images, PDF, ZIP, gzip, "
                 "POSIX tar archives, shell scripts (#! shebang), ASCII text, "
                 "directories, symlinks, devices, FIFOs, and sockets. "
                 "Uses magic bytes — does not require libmagic.",
    .run         = file_run,
    .print_usage = file_print_usage,
};

void register_file_command(void) {
    register_command(&cmd_file_spec);
}
