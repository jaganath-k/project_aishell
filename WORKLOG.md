# aishell Week 3 — AI Work Log

**Date:** 2026-05-08
**Project:** aishell — BusyBox-style shell in C
**Branch:** week3 / week4
**Engineer:** Jagan
**AI Assistant:** Claude (claude-sonnet-4-6)

---

## Overview

This week extended aishell from a pure shell with 32 built-in commands into a system with a full package manager (`pkg`) and an HTTP registry server. The goal was to build the equivalent of `apt`/`npm` from first principles using C (POSIX), Node.js, and shell scripting — no package manager libraries used.

---

## Goals for the Week

| # | Goal | Status |
|---|------|--------|
| 1 | Design and implement `pkg` CLI with subcommand dispatch | ✅ Done |
| 2 | `pkg build` — archive source into distributable `.tar.gz` | ✅ Done |
| 3 | `pkg install` — extract, symlink, and track in local database | ✅ Done |
| 4 | `pkg list` — display all installed packages | ✅ Done |
| 5 | `pkg remove` — clean uninstall with symlink removal | ✅ Done |
| 6 | Integrate `pkg` into the aishell REPL | ✅ Done |
| 7 | Build HTTP registry server (Node.js + Express) | ✅ Done |
| 8 | `pkg check-update` — query registry, compare versions | ✅ Done |
| 9 | `pkg upgrade` — download newer version, swap install | ✅ Done |

---

## Day-by-Day Log

### Session 1 — pkg skeleton and `pkg build`

**Work done:**
- Created `pkg.c` from scratch with a subcommand dispatch table (`subcmd_t subcmds[]`)
- Implemented `pkg build <src-dir> <output.tar.gz>` using `fork + execlp + waitpid` calling system `tar`
- Defined `pkg.json` as the package metadata format (name, version, description, files, docs)
- Created `week3/pkg.json` for the aishell package itself

**First compile:**
```sh
gcc -Wall -Wextra -std=c11 -o pkg pkg.c
```

**First build command:**
```sh
./pkg build . ~/aishell-1.0.tar.gz
```
```
pkg: building from /home/jagan/aishell/week3
pkg: found pkg.json → aishell 1.0
pkg: archive → /home/jagan/aishell-1.0.tar.gz
```

**Problem encountered:** Archive included `.git/` directory and all its objects, making it unnecessarily large and leaking repository internals.

**Fix:** Added `--exclude=.git` and `--exclude=<output-basename>` to the `tar` invocation:
```c
char *tar_args[] = {
    "tar",
    "--exclude=.git",
    "--exclude=aishell-1.0.tar.gz",
    "-czf", out,
    "-C", src_dir, ".",
    NULL
};
```

---

### Session 2 — `pkg install`, `pkg list`, `pkg remove`

**Work done:**
- Implemented `pkg install <archive.tar.gz>` with full install pipeline
- Defined install layout under `~/.mysh/`
- Implemented `pkgdb.txt` as a plain-text package database
- Implemented `pkg list` reading pkgdb.txt and formatting a table
- Implemented `pkg remove <name>` with symlink cleanup and database update

**Install layout designed:**
```
~/.mysh/
├── bin/                          ← symlinks to executables
│   └── aishell → ../pkgs/aishell-2.0/aishell
├── pkgs/
│   └── aishell-2.0/              ← extracted package files
│       ├── pkg.json
│       ├── aishell
│       └── README.md
└── pkgdb.txt                     ← one line per package: "name version"
```

**Key implementation: two-pass extraction**

Install needs the package name and version (from inside `pkg.json`) before it knows where to extract. Solved by extracting to a temp dir first:
```c
mkdtemp(tmpdir);                          // /tmp/pkg_XXXXXX
tar --strip-components=1 -xzf ... tmpdir  // extract temp
parse_pkgjson(tmpdir + "/pkg.json")        // read name, version
tar --strip-components=1 -xzf ... install_dir  // extract final
```

**`--strip-components=1` rationale:** Different `tar` invocations produce archives with either `./` or `<name>/` as root. `--strip-components=1` normalizes both so files always land directly in the target directory.

**`pkg install` output:**
```sh
./pkg install ~/aishell-2.0.tar.gz
```
```
pkg: installing aishell-2.0
pkg:   linked /home/jagan/.mysh/bin/aishell -> /home/jagan/.mysh/pkgs/aishell-2.0/aishell
pkg: aishell-2.0 installed successfully
```

**`pkg list` output:**
```sh
./pkg list
```
```
NAME                     VERSION
------------------------ -------
aishell                  2.0
```

**`pkg remove` output:**
```sh
./pkg remove aishell
```
```
pkg:   removed /home/jagan/.mysh/bin/aishell
pkg: aishell-2.0 removed
```

**Problem encountered (Permission denied):** Running `pkg build` as root previously left `/tmp/aishell-1.0.tar.gz` owned by root. Re-running as `jagan` failed with EACCES when tar tried to overwrite it.

**Fix:** Added `unlink(out)` before the `tar` call to remove any stale file before writing:
```c
unlink(out);  // ignore error — file may not exist yet
run_command(tar_args);
```

---

### Session 3 — Registry Server (Node.js + Express)

**Work done:**
- Created `week3/registry/server.js` — minimal HTTP registry
- Two endpoints: `GET /packages` (list all) and `GET /packages/:name` (single lookup)
- Static file serving from `registry/files/` for tarball downloads
- Created `registry/package.json` with express dependency
- Updated `.gitignore` to exclude `registry/node_modules/` and `registry/files/*.tar.gz`
- Moved registry folder to `week3/registry/` for inclusion in the week4 PR

**Server setup:**
```sh
cd ~/aishell/week3/registry
npm install
node server.js
```
```
aishell registry  →  http://localhost:3000
  GET /packages
  GET /packages/:name
  GET /files/<archive>.tar.gz
```

**Endpoint responses:**

```sh
curl -s http://localhost:3000/packages
```
```json
[
  {"name":"hello","latestVersion":"1.0.0","downloadUrl":"http://localhost:3000/files/hello-1.0.0.tar.gz"},
  {"name":"wcplus","latestVersion":"2.1.0","downloadUrl":"http://localhost:3000/files/wcplus-2.1.0.tar.gz"},
  {"name":"aishell","latestVersion":"2.0","downloadUrl":"http://localhost:3000/files/aishell-2.0.tar.gz"}
]
```

```sh
curl -s http://localhost:3000/packages/aishell
```
```json
{"name":"aishell","latestVersion":"2.0","downloadUrl":"http://localhost:3000/files/aishell-2.0.tar.gz","description":"BusyBox-style shell with 32 built-in commands"}
```

```sh
curl -s http://localhost:3000/packages/unknown
```
```json
{"error":"package 'unknown' not found"}
```

**Clarification on how the registry works:**

A common misconception: copying a `.tar.gz` into `registry/files/` does NOT automatically register the package. Two steps are always required:
1. Copy archive to `registry/files/<name>-<version>.tar.gz`
2. Add an entry to `packages[]` in `server.js` and restart the server

The server is an information broker — `pkg` clients pull from it on demand; nothing is pushed automatically.

---

### Session 4 — Shell Integration

**Work done:**
- Modified `aishell_main.c` to prepend `~/.mysh/bin` to `$PATH` at startup
- Added `exec_external()` function for fork+execvp dispatch
- Wired `pkg` keyword in the REPL to dispatch to the external `pkg` binary

**PATH augmentation (called once at startup):**
```c
static void prepend_mysh_bin_to_path(void) {
    const char *home = getenv("HOME");
    if (!home) return;
    char mysh_bin[512];
    snprintf(mysh_bin, sizeof(mysh_bin), "%s/.mysh/bin", home);
    const char *old = getenv("PATH");
    char new_path[4096];
    if (old && old[0])
        snprintf(new_path, sizeof(new_path), "%s:%s", mysh_bin, old);
    else
        snprintf(new_path, sizeof(new_path), "%s", mysh_bin);
    setenv("PATH", new_path, 1);
}
```

**Dispatch logic in execute_command():**
```c
if (spec) {
    ret = spec->run(cmd->argc, cmd->argv);          // built-in
} else if (strcmp(cmdname, "pkg") == 0) {
    ret = exec_external(cmd);                       // external binary
} else {
    fprintf(stderr, "aishell: %s: command not found\n", cmd->argv[0]);
    ret = 127;
}
```

**Demo inside the aishell REPL:**
```
$ make && ./aishell
aishell> pkg list
NAME                     VERSION
------------------------ -------
aishell                  2.0

aishell> pkg check-update aishell
aishell 2.0 is up to date

aishell> exit
```

---

### Session 5 — `pkg check-update` and `pkg upgrade`

**Work done:**
- Implemented `pkg check-update <name>` — queries registry, compares versions, reports status
- Implemented `pkg upgrade <name>` — full download-remove-reinstall cycle
- Implemented `capture_command()` — pipe-based stdout capture (used for curl output)
- Implemented `version_cmp()` — numeric dot-segment comparison

**`capture_command()` — stdout capture via pipe:**
```c
static int capture_command(char *const args[], char *buf, size_t bufsz) {
    int pipefd[2];
    pipe(pipefd);
    pid_t pid = fork();
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
    size_t total = 0; ssize_t n;
    while (total < bufsz - 1 &&
           (n = read(pipefd[0], buf + total, bufsz - 1 - total)) > 0)
        total += (size_t)n;
    buf[total] = '\0';
    close(pipefd[0]);
    int status; waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
```

**`version_cmp()` — numeric comparison:**

String comparison would give wrong results: `"10.0" < "2.0"` lexicographically. Numeric comparison is required:
```c
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
```

**`pkg check-update` output:**

When update is available:
```sh
./pkg check-update aishell
```
```
Update available: aishell  1.0 -> 2.0
  Run: pkg upgrade aishell
```

When up to date:
```sh
./pkg check-update aishell
```
```
aishell 2.0 is up to date
```

**`pkg upgrade` — full end-to-end output:**

Setup (install old version):
```sh
# pkg.json: "version": "1.0"
./pkg build . ~/aishell-1.0.tar.gz
./pkg install ~/aishell-1.0.tar.gz
./pkg list
```
```
NAME                     VERSION
------------------------ -------
aishell                  1.0
```

Run upgrade:
```sh
./pkg upgrade aishell
```
```
pkg: upgrading aishell  1.0 -> 2.0
pkg: downloading http://localhost:3000/files/aishell-2.0.tar.gz ...
pkg: removing aishell-1.0 ...
pkg:   removed /home/jagan/.mysh/bin/aishell
pkg: aishell-1.0 removed
pkg: installing aishell-2.0
pkg:   linked /home/jagan/.mysh/bin/aishell -> /home/jagan/.mysh/pkgs/aishell-2.0/aishell
pkg: aishell-2.0 installed successfully
```

Verify:
```sh
./pkg list
```
```
NAME                     VERSION
------------------------ -------
aishell                  2.0
```

```sh
./pkg check-update aishell
```
```
aishell 2.0 is up to date
```

**`pkg upgrade` internal flow:**
```
pkg upgrade aishell
    │
    ├─ curl registry/packages/aishell  → latestVersion="2.0", downloadUrl
    ├─ pkgdb_lookup("aishell")         → installed="1.0"
    ├─ version_cmp("2.0","1.0") > 0   → proceed
    │
    ├─ mkdtemp /tmp/pkg_up_XXXXXX/
    ├─ curl -o /tmp/.../download.tar.gz  <downloadUrl>
    │
    ├─ cmd_remove("aishell")      → unlink symlinks, rm -rf old dir, update pkgdb
    ├─ cmd_install(tmp_tarball)   → extract to ~/.mysh/pkgs/, symlink, pkgdb append
    │
    └─ rm -rf /tmp/pkg_up_XXXXXX/
```

---

## Problems Encountered and How They Were Solved

### 1. `.git/` objects in built archive
- **Symptom:** `tar -tzf aishell.tar.gz` showed `.git/objects/...` in the listing
- **Cause:** `tar` includes everything under the source directory by default
- **Fix:** Added `--exclude=.git` to tar arguments in `cmd_build()`

### 2. "Permission denied" when building tarball
- **Symptom:** `pkg build` failed with EACCES on `/tmp/aishell-1.0.tar.gz`
- **Cause:** Previous run as `root` left a stale file with root ownership; current user `jagan` could not overwrite it
- **Fix:** Added `unlink(out)` before the `tar` call. If the file doesn't exist, `unlink` returns ENOENT which is harmless. If it's owned by another user, instruct the user to choose a path they own (e.g., under `$HOME`)

### 3. GCC `-Wformat-truncation` warnings
- **Symptom:** Dozens of warnings: `output may be truncated copying N bytes of 'buf'`
- **Cause:** GCC performs worst-case buffer analysis on `snprintf("%s/suffix", base)`. If `base` is declared `char[512]`, GCC assumes it can be 511 bytes, making the total exceed the destination buffer size
- **Fix:** Defined separate semantically-sized constants:
  ```c
  #define PKG_BASE_LEN  256   // ~/.mysh path component
  #define PKG_PATH_MAX  512   // single path segment
  #define PKG_FULL_MAX 1024   // two-segment constructed paths
  ```
  Sized each buffer to the actual worst-case sum of its inputs

### 4. `realpath()` implicit declaration warning
- **Symptom:** `warning: implicit declaration of function 'realpath'`
- **Cause:** `#define _POSIX_C_SOURCE 200809L` does not expose `realpath` on this glibc version
- **Fix:** Changed feature macro to `#define _GNU_SOURCE` which exposes the full GNU/POSIX surface

### 5. Archive root varies between `./` and `<name>/`
- **Symptom:** Sometimes files landed in `aishell-2.0/aishell/`, sometimes in `aishell-2.0/` directly
- **Cause:** `tar -czf out -C srcdir .` produces `./` root; user-built tarballs sometimes use `name/` root
- **Fix:** Always pass `--strip-components=1` during both the temp extraction and the final installation, which removes exactly one path component regardless of what it is

### 6. Version number mismatch after upgrade
- **Symptom:** After `pkg upgrade`, `pkg list` showed `1.0` instead of `2.0`
- **Cause:** `pkg.json` inside the `aishell-2.0.tar.gz` archive still said `"version": "1.0"` — the archive was built before updating `pkg.json`
- **Fix:** Updated `pkg.json` to `"version": "2.0"` before rebuilding the archive and placing it in `registry/files/`

### 7. Registry appeared to "do nothing" when a file was copied in
- **Symptom:** Copied `.tar.gz` to `registry/files/` inside the shell, nothing happened
- **Cause:** The registry server is a static information broker. `pkg` clients query it on demand; there is no file-watcher or event system
- **Fix (understanding):** Two manual steps always required: (1) file in `registry/files/`, (2) entry in `packages[]` in `server.js`, followed by a server restart. `pkg upgrade` then actively fetches the info

---

## Architecture Decisions

| Decision | Rationale |
|----------|-----------|
| Plain-text `pkgdb.txt` (not SQLite) | Zero external dependencies, human-readable, sufficient for the scale |
| Minimal JSON parser (strstr-based) | No external library needed for reading 5-field `pkg.json` |
| `curl` for HTTP (subprocess) | Avoids libcurl linking complexity; `capture_command()` captures stdout via pipe |
| `--strip-components=1` always | Normalizes archive roots across different build tools |
| Two-pass extraction (temp then final) | Metadata must be read before install path is known |
| `unlink()` before `tar` write | Handles stale files from previous runs or different users |
| `#define _GNU_SOURCE` | Needed for `realpath()` on glibc; exposes the full API surface |
| `~/.mysh/bin` in PATH at shell startup | Makes installed packages available as shell commands without user configuration |
| External `pkg` binary (not built-in) | Keeps pkg logic separate from shell; can be updated independently |

---

## Files Created / Modified This Week

| File | Type | Lines | Description |
|------|------|-------|-------------|
| `week3/pkg.c` | Created | 838 | Full package manager — 6 subcommands |
| `week3/pkg.json` | Created | 8 | aishell package metadata |
| `week3/aishell_main.c` | Modified | — | Added PATH augmentation + `pkg` dispatch |
| `week3/registry/server.js` | Created | 82 | Express HTTP registry server |
| `week3/registry/package.json` | Created | — | npm metadata, express dependency |
| `week3/.gitignore` | Modified | — | Added `registry/node_modules/`, `registry/files/*.tar.gz` |

---

## pkg Command Reference

| Command | Description | Example |
|---------|-------------|---------|
| `pkg build <dir> <out.tar.gz>` | Package source directory into archive | `./pkg build . ~/aishell-2.0.tar.gz` |
| `pkg install <archive.tar.gz>` | Install package from local archive | `./pkg install ~/aishell-2.0.tar.gz` |
| `pkg list` | Show all installed packages | `./pkg list` |
| `pkg remove <name>` | Uninstall a package | `./pkg remove aishell` |
| `pkg check-update <name>` | Check if registry has a newer version | `./pkg check-update aishell` |
| `pkg upgrade <name>` | Download and install latest version | `./pkg upgrade aishell` |

---

## How to Run Everything

```sh
# 1. Compile
cd ~/aishell/week3
gcc -Wall -Wextra -std=c11 -o pkg pkg.c

# 2. Start the registry server (keep running in a separate terminal)
cd ~/aishell/week3/registry
npm install          # first time only
node server.js

# 3. Build a package
cd ~/aishell/week3
./pkg build . ~/aishell-2.0.tar.gz

# 4. Place tarball in registry
cp ~/aishell-2.0.tar.gz registry/files/
# (restart node server.js after updating server.js packages[] entry)

# 5. Install, check, upgrade
./pkg install ~/aishell-1.0.tar.gz   # install old version first
./pkg list
./pkg check-update aishell
./pkg upgrade aishell
./pkg list

# 6. Run inside the shell
make && ./aishell
aishell> pkg list
aishell> pkg check-update aishell
aishell> exit
```

---

## Week 3 Outcome

A self-contained, working package ecosystem built from scratch:
- A C binary (`pkg`) that manages the full lifecycle of packages (build → install → list → check-update → upgrade → remove)
- A local install database (`~/.mysh/pkgdb.txt`) tracking installed packages and versions
- An HTTP registry server (Node.js) that acts as the source of truth for available packages and download URLs
- Shell integration so installed packages are usable as first-class shell commands inside aishell

No external package management libraries were used. All HTTP interaction goes through the system `curl` binary; all archive creation/extraction goes through system `tar`. The only non-standard dependency is `express` for the registry server.

---

*Generated: 2026-05-08 | aishell week3 | AI-assisted development log*
