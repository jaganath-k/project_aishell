/*
 * pkg.c — package manager for aishell
 *
 * Subcommands:
 *   pkg build        <src-dir> <output.tar.gz>
 *   pkg install      <package.tar.gz>
 *   pkg list
 *   pkg remove       <name>
 *   pkg check-update <name>
 *   pkg upgrade      <name>
 *
 * Compile:
 *   gcc -Wall -Wextra -std=c11 -o pkg pkg.c
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>
#include <fcntl.h>
#include <errno.h>

#define REGISTRY_URL "http://localhost:3000"

/* =====================================================================
 * Constants and data structures
 *
 * Buffer sizing rationale (silences -Wformat-truncation):
 *   PKG_BASE_LEN  – mysh_base only: HOME (≤248) + "/.mysh" + NUL
 *   PKG_PATH_MAX  – single-segment constructed paths (base + /pkgs/name-ver)
 *   PKG_FULL_MAX  – two-segment paths (install_dir + "/" + filename)
 * =================================================================== */

#define PKG_MAX_FILES  64
#define PKG_NAME_MAX  128   /* package name, filename — short identifiers */
#define PKG_VER_MAX    64   /* version string */
#define PKG_DESC_MAX  256   /* description */
#define PKG_BASE_LEN  256   /* mysh_base: HOME + "/.mysh" */
#define PKG_PATH_MAX  512   /* install_dir, bin_dir, pkgdb path */
#define PKG_FULL_MAX 1024   /* install_dir + "/" + filename */

/* Parsed contents of a package's pkg.json */
typedef struct {
    char name[PKG_NAME_MAX];
    char version[PKG_VER_MAX];
    char description[PKG_DESC_MAX];
    char files[PKG_MAX_FILES][PKG_NAME_MAX]; /* declared executable basenames */
    int  nfiles;
} pkg_meta_t;

/* =====================================================================
 * Subcommand dispatch table
 * =================================================================== */

typedef int (*subcmd_fn)(int argc, char **argv);

typedef struct {
    const char *name;
    const char *usage;
    subcmd_fn   fn;
} subcmd_t;

static int cmd_build(int argc, char **argv);
static int cmd_install(int argc, char **argv);
static int cmd_list(int argc, char **argv);
static int cmd_remove(int argc, char **argv);
static int cmd_check_update(int argc, char **argv);
static int cmd_upgrade(int argc, char **argv);

static const subcmd_t subcmds[] = {
    { "build",        "pkg build <src-dir> <output.tar.gz>", cmd_build        },
    { "install",      "pkg install <package.tar.gz>",         cmd_install      },
    { "list",         "pkg list",                             cmd_list         },
    { "remove",       "pkg remove <name>",                   cmd_remove       },
    { "check-update", "pkg check-update <name>",             cmd_check_update },
    { "upgrade",      "pkg upgrade <name>",                  cmd_upgrade      },
    { NULL, NULL, NULL }
};

/* =====================================================================
 * Path helpers
 * =================================================================== */

/* Expand a leading '~' to $HOME. */
static void expand_home(const char *path, char *buf, size_t bufsz) {
    if (path[0] == '~') {
        const char *home = getenv("HOME");
        if (!home) home = "/tmp";
        snprintf(buf, bufsz, "%s%s", home, path + 1);
    } else {
        snprintf(buf, bufsz, "%s", path);
    }
}

/* Copy path into buf, stripping trailing '/' characters. */
static void strip_slash(const char *path, char *buf, size_t bufsz) {
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') len--;
    if (len >= bufsz) len = bufsz - 1;
    memcpy(buf, path, len);
    buf[len] = '\0';
}

/* Return pointer to the last path component; does not allocate. */
static const char *path_base(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* Write the parent directory of path into buf[bufsz]. */
static void path_dir(const char *path, char *buf, size_t bufsz) {
    const char *s = strrchr(path, '/');
    if (!s)        { snprintf(buf, bufsz, "."); return; }
    if (s == path) { snprintf(buf, bufsz, "/"); return; }
    size_t len = (size_t)(s - path);
    if (len >= bufsz) len = bufsz - 1;
    memcpy(buf, path, len);
    buf[len] = '\0';
}

/* =====================================================================
 * mkdirp — create a directory and all missing parent components
 * =================================================================== */
static int mkdirp(const char *path) {
    char tmp[PKG_PATH_MAX];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    return (mkdir(tmp, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}

/* =====================================================================
 * Process helper — fork + execvp + waitpid
 * =================================================================== */
static int run_command(char *const args[]) {
    pid_t pid = fork();
    if (pid < 0) { perror("pkg: fork"); return -1; }
    if (pid == 0) { execvp(args[0], args); perror(args[0]); _exit(127); }
    int status;
    if (waitpid(pid, &status, 0) < 0) { perror("pkg: waitpid"); return -1; }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* =====================================================================
 * capture_command — fork + execvp, capture stdout into buf[bufsz].
 *
 * stderr is routed to /dev/null so curl progress bars and error lines
 * don't pollute the shell output; we check the response ourselves.
 * Returns the child exit status (0 = success), or -1 on fork/wait error.
 * =================================================================== */
static int capture_command(char *const args[], char *buf, size_t bufsz) {
    int pipefd[2];
    if (pipe(pipefd) != 0) { perror("pkg: pipe"); return -1; }

    pid_t pid = fork();
    if (pid < 0) {
        perror("pkg: fork");
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, STDERR_FILENO); close(dn); }
        execvp(args[0], args);
        _exit(127);
    }

    close(pipefd[1]);
    size_t total = 0;
    ssize_t n;
    while (total < bufsz - 1 &&
           (n = read(pipefd[0], buf + total, bufsz - 1 - total)) > 0)
        total += (size_t)n;
    buf[total] = '\0';
    close(pipefd[0]);

    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* =====================================================================
 * version_cmp — compare dot-separated version strings numerically.
 * Returns < 0 if a < b, 0 if equal, > 0 if a > b.
 * Handles "1.0", "1.0.0", "2.1.3" and mixed-depth like "1.0" vs "1.0.0".
 * =================================================================== */
static int version_cmp(const char *a, const char *b) {
    while (*a || *b) {
        int va = 0, vb = 0;
        while (*a && *a != '.') va = va * 10 + (*a++ - '0');
        while (*b && *b != '.') vb = vb * 10 + (*b++ - '0');
        if (va != vb) return va - vb;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

/* =====================================================================
 * Minimal JSON helpers — reads our controlled pkg.json format only
 * =================================================================== */

/* Read entire file into a heap-allocated NUL-terminated buffer.
 * Caller must free(). Returns NULL on error. */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf)   { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* Extract the first string value for "key" from a JSON buffer.
 * Returns length written, or -1 if the key is absent. */
static int json_get_string(const char *buf, const char *key,
                             char *out, size_t outsz) {
    char needle[PKG_NAME_MAX + 4];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(buf, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < outsz - 1) out[i++] = *p++;
    out[i] = '\0';
    return (int)i;
}

/* Extract all string values from the JSON array for "key".
 * Returns the count of items written into out[][PKG_NAME_MAX]. */
static int json_get_array(const char *buf, const char *key,
                            char out[][PKG_NAME_MAX], int max) {
    char needle[PKG_NAME_MAX + 4];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(buf, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p && *p != '[') p++;
    if (!*p) return 0;
    p++;
    int count = 0;
    while (*p && *p != ']' && count < max) {
        while (*p && *p != '"' && *p != ']') p++;
        if (*p != '"') break;
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i < (size_t)(PKG_NAME_MAX - 1))
            out[count][i++] = *p++;
        out[count][i] = '\0';
        if (*p == '"') p++;
        count++;
    }
    return count;
}

/* Parse pkg.json at path into meta. Returns 0 on success.
 * Fails if file is unreadable or "name"/"version" are absent. */
static int parse_pkgjson(const char *path, pkg_meta_t *meta) {
    char *buf = read_file(path);
    if (!buf) {
        fprintf(stderr, "pkg: cannot read %s: %s\n", path, strerror(errno));
        return -1;
    }
    memset(meta, 0, sizeof(*meta));
    int ok = (json_get_string(buf, "name",    meta->name,    sizeof(meta->name))    >= 0 &&
              json_get_string(buf, "version", meta->version, sizeof(meta->version)) >= 0);
    json_get_string(buf, "description", meta->description, sizeof(meta->description));
    meta->nfiles = json_get_array(buf, "files", meta->files, PKG_MAX_FILES);
    free(buf);
    if (!ok) { fprintf(stderr, "pkg: pkg.json missing 'name' or 'version'\n"); }
    return ok ? 0 : -1;
}

/* =====================================================================
 * find_pkgjson — search for pkg.json at depth 0 or 1 inside dir.
 * Writes the found absolute path into out[outsz]. Returns 0 on success.
 * =================================================================== */
static int find_pkgjson(const char *dir, char *out, size_t outsz) {
    struct stat st;

    /* Depth 0: dir/pkg.json */
    snprintf(out, outsz, "%s/pkg.json", dir);
    if (stat(out, &st) == 0) return 0;

    /* Depth 1: dir/<entry>/pkg.json */
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *ent;
    int found = -1;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        snprintf(out, outsz, "%s/%s/pkg.json", dir, ent->d_name);
        if (stat(out, &st) == 0) { found = 0; break; }
    }
    closedir(d);
    return found;
}

/* =====================================================================
 * pkgdb helpers
 *
 * ~/.mysh/pkgdb.txt format: one line per package
 *   <name> <version>
 * =================================================================== */

static int pkgdb_append(const char *mysh_base, const char *name,
                         const char *version) {
    char path[PKG_PATH_MAX];
    snprintf(path, sizeof(path), "%s/pkgdb.txt", mysh_base);
    FILE *f = fopen(path, "a");
    if (!f) { perror("pkg: pkgdb"); return -1; }
    fprintf(f, "%s %s\n", name, version);
    fclose(f);
    return 0;
}

/* Look up name → version in pkgdb. Returns 0 if found, -1 otherwise. */
static int pkgdb_lookup(const char *mysh_base, const char *name,
                         char *ver_out, size_t ver_sz) {
    char path[PKG_PATH_MAX];
    snprintf(path, sizeof(path), "%s/pkgdb.txt", mysh_base);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256], n[PKG_NAME_MAX], v[PKG_VER_MAX];
    int found = -1;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%127s %63s", n, v) == 2 && strcmp(n, name) == 0) {
            strncpy(ver_out, v, ver_sz - 1);
            ver_out[ver_sz - 1] = '\0';
            found = 0; break;
        }
    }
    fclose(f);
    return found;
}

/* Remove all lines for name from pkgdb.txt (rewrite without them). */
static int pkgdb_remove_entry(const char *mysh_base, const char *name) {
    char path[PKG_PATH_MAX];
    snprintf(path, sizeof(path), "%s/pkgdb.txt", mysh_base);
    FILE *f = fopen(path, "r");
    if (!f) return 0;  /* nothing to remove */

    char kept[256][256];
    int  nkept = 0;
    char line[256], n[PKG_NAME_MAX];
    while (fgets(line, sizeof(line), f) && nkept < 256) {
        if (sscanf(line, "%127s", n) == 1 && strcmp(n, name) == 0) continue;
        strncpy(kept[nkept++], line, 255);
    }
    fclose(f);

    f = fopen(path, "w");
    if (!f) return -1;
    for (int i = 0; i < nkept; i++) fputs(kept[i], f);
    fclose(f);
    return 0;
}

/* =====================================================================
 * pkg build <src-dir> <output.tar.gz>
 *
 * Requires pkg.json inside <src-dir>.  Runs:
 *   tar --exclude=.git --exclude=<out-basename> -czf <out> -C <parent> <base>
 * =================================================================== */
static int cmd_build(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: pkg build <src-dir> <output.tar.gz>\n");
        return 1;
    }
    const char *src = argv[0];
    const char *out = argv[1];

    struct stat st;
    if (stat(src, &st) != 0) {
        fprintf(stderr, "pkg build: %s: %s\n", src, strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "pkg build: %s: not a directory\n", src);
        return 1;
    }

    char pkgjson[PKG_PATH_MAX];
    snprintf(pkgjson, sizeof(pkgjson), "%s/pkg.json", src);
    if (stat(pkgjson, &st) != 0) {
        fprintf(stderr, "pkg build: no pkg.json found in %s\n", src);
        return 1;
    }

    char norm[PKG_PATH_MAX];
    strip_slash(src, norm, sizeof(norm));
    char parent[PKG_PATH_MAX];
    path_dir(norm, parent, sizeof(parent));
    const char *base = path_base(norm);

    /* Exclude the output file in case it lands inside src-dir */
    const char *out_base = path_base(out);
    char exclude_out[PKG_PATH_MAX];
    snprintf(exclude_out, sizeof(exclude_out), "--exclude=%s", out_base);

    /* Remove stale output file so tar can create it fresh regardless of
     * who owns it; unlink failure (e.g. not present) is silently ignored. */
    unlink(out);

    char *tar_args[] = {
        "tar", "--exclude=.git", exclude_out,
        "-czf", (char *)out, "-C", parent, (char *)base, NULL
    };

    printf("pkg: building %s from %s/%s ...\n", out, parent, base);
    int r = run_command(tar_args);
    if (r == 0) printf("pkg: created %s\n", out);
    else        fprintf(stderr, "pkg: build failed (tar exited %d)\n", r);
    return r;
}

/* =====================================================================
 * pkg install <package.tar.gz>
 *
 * How it works:
 *  1. Extract tarball to /tmp/pkg_XXXXXX (--strip-components=1 normalises
 *     the archive root, whether it is "./" or "<name>/").
 *  2. Find and parse pkg.json → name, version, files[].
 *  3. Create ~/.mysh/pkgs/<name>-<version>/ and ~/.mysh/bin/.
 *  4. Extract again to the install dir (--strip-components=1).
 *  5. Symlink each files[] entry into ~/.mysh/bin/.
 *  6. Clean up the temp dir and append to ~/.mysh/pkgdb.txt.
 * =================================================================== */
static int cmd_install(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "usage: pkg install <package.tar.gz>\n");
        return 1;
    }
    const char *tarfile = argv[0];

    struct stat st;
    if (stat(tarfile, &st) != 0) {
        fprintf(stderr, "pkg install: %s: %s\n", tarfile, strerror(errno));
        return 1;
    }

    /* Resolve to absolute path before any chdir side-effects.
     * realpath() caps output at PATH_MAX and handles relative paths. */
    char tar_abs[PATH_MAX];
    if (!realpath(tarfile, tar_abs)) {
        fprintf(stderr, "pkg install: %s: %s\n", tarfile, strerror(errno));
        return 1;
    }

    /* ---- 1. Extract to temp dir ---- */
    char tmpdir[] = "/tmp/pkg_XXXXXX";
    if (!mkdtemp(tmpdir)) { perror("pkg install: mkdtemp"); return 1; }

    char *ex_tmp[] = {
        "tar", "--strip-components=1", "-xzf", tar_abs, "-C", tmpdir, NULL
    };
    if (run_command(ex_tmp) != 0) {
        fprintf(stderr, "pkg install: failed to extract %s\n", tarfile);
        char *rm[] = { "rm", "-rf", tmpdir, NULL };  run_command(rm);
        return 1;
    }

    /* ---- 2. Find and parse pkg.json ---- */
    char pkgjson_path[PKG_PATH_MAX];
    if (find_pkgjson(tmpdir, pkgjson_path, sizeof(pkgjson_path)) != 0) {
        fprintf(stderr, "pkg install: no pkg.json found in %s\n", tarfile);
        char *rm[] = { "rm", "-rf", tmpdir, NULL };  run_command(rm);
        return 1;
    }
    pkg_meta_t meta;
    if (parse_pkgjson(pkgjson_path, &meta) != 0) {
        char *rm[] = { "rm", "-rf", tmpdir, NULL };  run_command(rm);
        return 1;
    }

    printf("pkg: installing %s-%s\n", meta.name, meta.version);

    /* ---- 3. Prepare target directories ---- */
    /* mysh_base[256]: HOME(≤248) + "/.mysh" fits comfortably            */
    /* install_dir[512]: 255 + "/pkgs/" + 127 + "-" + 63 = 452 ✓        */
    /* bin_dir[512]: 255 + "/bin" = 259 ✓                                */
    char mysh_base[PKG_BASE_LEN];
    expand_home("~/.mysh", mysh_base, sizeof(mysh_base));

    char install_dir[PKG_PATH_MAX];
    snprintf(install_dir, sizeof(install_dir), "%s/pkgs/%s-%s",
             mysh_base, meta.name, meta.version);

    char bin_dir[PKG_PATH_MAX];
    snprintf(bin_dir, sizeof(bin_dir), "%s/bin", mysh_base);

    if (mkdirp(install_dir) != 0) {
        fprintf(stderr, "pkg install: cannot create %s: %s\n",
                install_dir, strerror(errno));
        char *rm[] = { "rm", "-rf", tmpdir, NULL };  run_command(rm);
        return 1;
    }
    if (mkdirp(bin_dir) != 0) {
        fprintf(stderr, "pkg install: cannot create %s: %s\n",
                bin_dir, strerror(errno));
        char *rm[] = { "rm", "-rf", tmpdir, NULL };  run_command(rm);
        return 1;
    }

    /* ---- 4. Extract to install dir ---- */
    char *ex_inst[] = {
        "tar", "--strip-components=1", "-xzf", tar_abs, "-C", install_dir, NULL
    };
    if (run_command(ex_inst) != 0) {
        fprintf(stderr, "pkg install: extraction to %s failed\n", install_dir);
        char *rm[] = { "rm", "-rf", tmpdir, NULL };  run_command(rm);
        return 1;
    }

    /* ---- 5. Create symlinks ---- */
    /* src/dst[1024]: install_dir(511) + "/" + filename(127) = 639 ✓ */
    int errors = 0;
    for (int i = 0; i < meta.nfiles; i++) {
        char src[PKG_FULL_MAX], dst[PKG_FULL_MAX];
        snprintf(src, sizeof(src), "%s/%s", install_dir, meta.files[i]);
        snprintf(dst, sizeof(dst), "%s/%s", bin_dir,     meta.files[i]);
        unlink(dst);  /* remove stale link silently */
        if (symlink(src, dst) != 0) {
            fprintf(stderr, "pkg install: symlink %s -> %s: %s\n",
                    dst, src, strerror(errno));
            errors++;
        } else {
            printf("pkg:   linked %s -> %s\n", dst, src);
        }
    }

    /* ---- 6. Cleanup + update db ---- */
    char *rm[] = { "rm", "-rf", tmpdir, NULL };  run_command(rm);
    pkgdb_append(mysh_base, meta.name, meta.version);

    printf("pkg: %s-%s installed%s\n",
           meta.name, meta.version, errors ? " (with errors)" : " successfully");
    return errors ? 1 : 0;
}

/* =====================================================================
 * pkg list
 *
 * Reads ~/.mysh/pkgdb.txt and prints a formatted NAME / VERSION table.
 * =================================================================== */
static int cmd_list(int argc, char **argv) {
    (void)argc; (void)argv;

    char mysh_base[PKG_BASE_LEN];
    expand_home("~/.mysh", mysh_base, sizeof(mysh_base));

    char path[PKG_PATH_MAX];   /* 255 + "/pkgdb.txt"(10) = 265 ✓ */
    snprintf(path, sizeof(path), "%s/pkgdb.txt", mysh_base);

    FILE *f = fopen(path, "r");
    if (!f) { printf("No packages installed.\n"); return 0; }

    printf("%-24s %s\n", "NAME", "VERSION");
    printf("%-24s %s\n", "------------------------", "-------");

    char line[256], name[PKG_NAME_MAX], version[PKG_VER_MAX];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%127s %63s", name, version) == 2) {
            printf("%-24s %s\n", name, version);
            count++;
        }
    }
    fclose(f);
    if (count == 0) printf("No packages installed.\n");
    return 0;
}

/* =====================================================================
 * pkg remove <name>
 *
 * How it works:
 *  1. Look up <name> in pkgdb to get the installed version.
 *  2. Read pkg.json from the install dir to obtain the files[] list.
 *  3. unlink() each symlink from ~/.mysh/bin/.
 *  4. rm -rf the install dir ~/.mysh/pkgs/<name>-<version>/.
 *  5. Rewrite pkgdb.txt without the removed entry.
 * =================================================================== */
static int cmd_remove(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "usage: pkg remove <name>\n");
        return 1;
    }
    const char *name = argv[0];

    char mysh_base[PKG_BASE_LEN];
    expand_home("~/.mysh", mysh_base, sizeof(mysh_base));

    /* ---- 1. Lookup ---- */
    char version[PKG_VER_MAX];
    if (pkgdb_lookup(mysh_base, name, version, sizeof(version)) != 0) {
        fprintf(stderr, "pkg remove: %s: not installed\n", name);
        return 1;
    }

    char install_dir[PKG_PATH_MAX];
    snprintf(install_dir, sizeof(install_dir), "%s/pkgs/%s-%s",
             mysh_base, name, version);

    printf("pkg: removing %s-%s ...\n", name, version);

    /* ---- 2. Read files[] from installed pkg.json ---- */
    char pkgjson_path[PKG_FULL_MAX];   /* install_dir(511) + "/pkg.json" = 520 ✓ */
    snprintf(pkgjson_path, sizeof(pkgjson_path), "%s/pkg.json", install_dir);

    pkg_meta_t meta;
    if (parse_pkgjson(pkgjson_path, &meta) == 0) {
        /* ---- 3. Remove symlinks ---- */
        char bin_dir[PKG_PATH_MAX];
        snprintf(bin_dir, sizeof(bin_dir), "%s/bin", mysh_base);
        for (int i = 0; i < meta.nfiles; i++) {
            char dst[PKG_FULL_MAX];   /* bin_dir(511) + "/" + name(127) = 639 ✓ */
            snprintf(dst, sizeof(dst), "%s/%s", bin_dir, meta.files[i]);
            if (unlink(dst) == 0)
                printf("pkg:   removed %s\n", dst);
            else if (errno != ENOENT)
                fprintf(stderr, "pkg:   unlink %s: %s\n", dst, strerror(errno));
        }
    }

    /* ---- 4. Remove install dir ---- */
    char *rm[] = { "rm", "-rf", install_dir, NULL };
    if (run_command(rm) != 0) {
        fprintf(stderr, "pkg remove: failed to remove %s\n", install_dir);
        return 1;
    }

    /* ---- 5. Update pkgdb ---- */
    pkgdb_remove_entry(mysh_base, name);

    printf("pkg: %s-%s removed\n", name, version);
    return 0;
}

/* =====================================================================
 * pkg check-update <name>
 *
 * 1. curl GET REGISTRY_URL/packages/<name>  →  JSON response
 * 2. Parse latestVersion from JSON
 * 3. Look up installed version in ~/.mysh/pkgdb.txt
 * 4. Print "up to date" or "Update available: old → new"
 * =================================================================== */
static int cmd_check_update(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "usage: pkg check-update <name>\n");
        return 1;
    }
    const char *name = argv[0];

    /* ---- 1. Query registry ---- */
    char url[PKG_FULL_MAX];
    snprintf(url, sizeof(url), REGISTRY_URL "/packages/%s", name);

    char resp[4096] = {0};
    char *curl_args[] = { "curl", "-s", url, NULL };
    if (capture_command(curl_args, resp, sizeof(resp)) != 0 || resp[0] == '\0') {
        fprintf(stderr, "pkg check-update: cannot reach registry at " REGISTRY_URL "\n");
        return 1;
    }
    if (strstr(resp, "\"error\"")) {
        fprintf(stderr, "pkg check-update: '%s' not found in registry\n", name);
        return 1;
    }

    /* ---- 2. Parse latestVersion ---- */
    char latest[PKG_VER_MAX] = {0};
    if (json_get_string(resp, "latestVersion", latest, sizeof(latest)) < 0) {
        fprintf(stderr, "pkg check-update: unexpected registry response\n");
        return 1;
    }

    /* ---- 3. Lookup installed version ---- */
    char mysh_base[PKG_BASE_LEN];
    expand_home("~/.mysh", mysh_base, sizeof(mysh_base));

    char installed[PKG_VER_MAX] = {0};
    if (pkgdb_lookup(mysh_base, name, installed, sizeof(installed)) != 0) {
        printf("%s: not installed  (registry has %s)\n", name, latest);
        printf("  Run: pkg upgrade %s\n", name);
        return 0;
    }

    /* ---- 4. Report ---- */
    int cmp = version_cmp(latest, installed);
    if (cmp > 0)
        printf("Update available: %s  %s -> %s\n  Run: pkg upgrade %s\n",
               name, installed, latest, name);
    else if (cmp == 0)
        printf("%s %s is up to date\n", name, installed);
    else
        printf("%s: installed %s is ahead of registry %s\n",
               name, installed, latest);
    return 0;
}

/* =====================================================================
 * pkg upgrade <name>
 *
 * 1. Query registry (same as check-update)
 * 2. Skip if already at latest version
 * 3. curl -o /tmp/… <downloadUrl>
 * 4. pkg remove <name>  (if currently installed)
 * 5. cmd_install() on the downloaded tarball
 * 6. Clean up temp dir
 * =================================================================== */
static int cmd_upgrade(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "usage: pkg upgrade <name>\n");
        return 1;
    }
    const char *name = argv[0];

    /* ---- 1. Query registry ---- */
    char url[PKG_FULL_MAX];
    snprintf(url, sizeof(url), REGISTRY_URL "/packages/%s", name);

    char resp[4096] = {0};
    char *curl_args[] = { "curl", "-s", url, NULL };
    if (capture_command(curl_args, resp, sizeof(resp)) != 0 || resp[0] == '\0') {
        fprintf(stderr, "pkg upgrade: cannot reach registry at " REGISTRY_URL "\n");
        return 1;
    }
    if (strstr(resp, "\"error\"")) {
        fprintf(stderr, "pkg upgrade: '%s' not found in registry\n", name);
        return 1;
    }

    char latest[PKG_VER_MAX]  = {0};
    char dl_url[PKG_FULL_MAX] = {0};
    if (json_get_string(resp, "latestVersion", latest, sizeof(latest)) < 0 ||
        json_get_string(resp, "downloadUrl",   dl_url,  sizeof(dl_url))  < 0) {
        fprintf(stderr, "pkg upgrade: unexpected registry response\n");
        return 1;
    }

    /* ---- 2. Compare versions ---- */
    char mysh_base[PKG_BASE_LEN];
    expand_home("~/.mysh", mysh_base, sizeof(mysh_base));

    char installed[PKG_VER_MAX] = {0};
    int  is_installed = (pkgdb_lookup(mysh_base, name, installed, sizeof(installed)) == 0);

    if (is_installed && version_cmp(latest, installed) <= 0) {
        printf("%s %s is already up to date\n", name, installed);
        return 0;
    }

    if (is_installed)
        printf("pkg: upgrading %s  %s -> %s\n", name, installed, latest);
    else
        printf("pkg: installing %s %s from registry\n", name, latest);

    /* ---- 3. Download to temp dir ---- */
    char tmpdir[] = "/tmp/pkg_up_XXXXXX";
    if (!mkdtemp(tmpdir)) { perror("pkg upgrade: mkdtemp"); return 1; }

    char tmptar[PKG_FULL_MAX];
    snprintf(tmptar, sizeof(tmptar), "%s/download.tar.gz", tmpdir);

    printf("pkg: downloading %s ...\n", dl_url);
    char *dl_args[] = { "curl", "-s", "-o", tmptar, dl_url, NULL };
    if (run_command(dl_args) != 0) {
        fprintf(stderr, "pkg upgrade: download failed\n");
        char *rm[] = { "rm", "-rf", tmpdir, NULL };  run_command(rm);
        return 1;
    }

    /* ---- 4. Remove old install ---- */
    if (is_installed) {
        char *rm_argv[] = { (char *)name, NULL };
        cmd_remove(1, rm_argv);
    }

    /* ---- 5. Install ---- */
    char *inst_argv[] = { tmptar, NULL };
    int r = cmd_install(1, inst_argv);

    /* ---- 6. Cleanup ---- */
    char *rm[] = { "rm", "-rf", tmpdir, NULL };
    run_command(rm);
    return r;
}

/* =====================================================================
 * Usage + main
 * =================================================================== */

static void print_usage(void) {
    fprintf(stderr, "usage: pkg <subcommand> [args]\n\n");
    fprintf(stderr, "Subcommands:\n");
    for (const subcmd_t *s = subcmds; s->name; s++)
        fprintf(stderr, "  %-10s  %s\n", s->name, s->usage);
    fprintf(stderr, "\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { print_usage(); return 1; }
    const char *sub = argv[1];
    for (const subcmd_t *s = subcmds; s->name; s++) {
        if (strcmp(sub, s->name) == 0)
            return s->fn(argc - 2, argv + 2);
    }
    fprintf(stderr, "pkg: unknown subcommand '%s'\n\n", sub);
    print_usage();
    return 1;
}
