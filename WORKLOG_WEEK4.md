# aishell Week 4 — AI Work Log

**Date:** 2026-05-13
**Project:** aishell — BusyBox-style shell in C
**Branch:** week4
**Engineer:** Jagan
**AI Assistant:** Claude (claude-sonnet-4-6)

---

## Overview

Week 4 had two parallel tracks:

1. **Text Editor Commands** — Five new built-in commands (`edit-show`, `edit-replace-line`, `edit-insert-line`, `edit-delete-line`, `edit-replace`) backed by a shared `edit_utils` library, giving aishell the ability to view and mutate files from the shell prompt without an external editor.

2. **Package Management System** — A full `pkg` CLI binary (6 subcommands: `build`, `install`, `list`, `remove`, `check-update`, `upgrade`), a local package database, and a Node.js + Express registry server — together forming a self-contained package ecosystem built from scratch.

Both tracks were integrated into the aishell REPL, making installed packages and file editing available as first-class shell commands.

---

## Goals for the Week

| # | Goal | Track | Status |
|---|------|-------|--------|
| 1 | Shared `edit_utils` library (read/write/atomic rename) | Editor | ✅ Done |
| 2 | `edit-show` — view file with line numbers and range | Editor | ✅ Done |
| 3 | `edit-replace-line` — replace an entire line by number | Editor | ✅ Done |
| 4 | `edit-insert-line` — insert a line before or after N | Editor | ✅ Done |
| 5 | `edit-delete-line` — delete a line or range | Editor | ✅ Done |
| 6 | `edit-replace` — regex find-and-replace across file | Editor | ✅ Done |
| 7 | `pkg build` — archive source dir into `.tar.gz` | pkg | ✅ Done |
| 8 | `pkg install` — extract, symlink, track in local DB | pkg | ✅ Done |
| 9 | `pkg list` — display all installed packages | pkg | ✅ Done |
| 10 | `pkg remove` — clean uninstall with symlink removal | pkg | ✅ Done |
| 11 | Registry server (Node.js + Express) | pkg | ✅ Done |
| 12 | `pkg check-update` — query registry, compare versions | pkg | ✅ Done |
| 13 | `pkg upgrade` — download newer version, swap install | pkg | ✅ Done |
| 14 | Shell integration — `pkg` dispatch + PATH augmentation | Shell | ✅ Done |

---

## Track 1 — Text Editor Commands

### Design: `edit_utils` shared library

Rather than duplicating file I/O in every editor command, a shared library was created:

**`edit_utils.h` / `edit_utils.c`** — four primitives used by all five commands:

```c
// Read all lines from a file into a heap-allocated array
char **edit_read_lines(const char *path, int *count);

// Write lines back atomically via temp file + rename
int edit_write_lines(const char *path, char **lines, int count);

// Free the lines array
void edit_free_lines(char **lines, int count);

// Return a copy of text guaranteed to end with '\n'
char *edit_ensure_newline(const char *text);
```

**Key design choice — atomic writes via temp file + rename:**
```c
int edit_write_lines(const char *path, char **lines, int count) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    for (int i = 0; i < count; i++)
        fputs(lines[i], f);
    fclose(f);
    if (rename(tmp, path) < 0) { remove(tmp); return -1; }
    return 0;
}
```
`rename()` is atomic on POSIX — if the process dies mid-write the original file is untouched. The `.tmp` file is never left behind on success.

**Dynamic buffer growth in `edit_read_lines`:**
```c
int cap = 256;
char **lines = malloc((size_t)cap * sizeof(char *));
while (fgets(buf, sizeof(buf), f)) {
    if (n == cap) { cap *= 2; lines = realloc(lines, ...); }
    lines[n++] = strdup(buf);
}
```
Starts at 256 lines, doubles on overflow — handles files of any size.

---

### Command 1: `edit-show`

**Purpose:** Display a file with 1-indexed line numbers, optionally restricted to a line range. Useful for previewing a file before running other `edit-*` commands.

**Source:** `cmd_edit_show.c`

**Options:**
| Flag | Long | Arg | Default | Description |
|------|------|-----|---------|-------------|
| `-h` | `--help` | — | — | show help and exit |
| — | `--help-json` | — | — | machine-readable help |
| `-n` | `--line` | N | 1 | first line to display |
| `-e` | `--end` | E | last | last line to display |
| — | — | FILE | required | file to display |

**Example — show whole file:**
```sh
./aishell edit-show test.txt
```
```
   1  Hello world
   2  This is a test file
   3  Line three
   4  Another line
   5  End of file
```

**Example — show only lines 2 to 4:**
```sh
./aishell edit-show -n 2 -e 4 test.txt
```
```
   2  This is a test file
   3  Line three
   4  Another line
```

**Usage:**
```
Usage: edit-show [-h] [--help-json] [-n N] [-e E] FILE
  display a file with line numbers (optionally a range)
```

---

### Command 2: `edit-replace-line`

**Purpose:** Replace the entire content of a specific line by line number. The line number is 1-indexed.

**Source:** `cmd_edit_replace_line.c`

**Options:**
| Flag | Long | Arg | Required | Description |
|------|------|-----|----------|-------------|
| `-h` | `--help` | — | No | show help |
| — | `--help-json` | — | No | machine-readable help |
| `-n` | `--line` | N | Yes | line number to replace |
| `-t` | `--text` | TEXT | Yes | replacement text |
| — | — | FILE | Yes | file to edit |

**Example:**
```sh
# Before
./aishell edit-show test.txt
   1  Hello world
   2  This is a test file
   3  Line three

# Replace line 2
./aishell edit-replace-line -n 2 -t "This line was replaced" test.txt

# After
./aishell edit-show test.txt
   1  Hello world
   2  This line was replaced
   3  Line three
```

**Usage:**
```
Usage: edit-replace-line [-h] [--help-json] -n N -t TEXT FILE
  replace a line in a file by line number
```

---

### Command 3: `edit-insert-line`

**Purpose:** Insert a new line before (default) or after a given line number. All subsequent lines shift down by one.

**Source:** `cmd_edit_insert_line.c`

**Options:**
| Flag | Long | Arg | Required | Description |
|------|------|-----|----------|-------------|
| `-h` | `--help` | — | No | show help |
| — | `--help-json` | — | No | machine-readable help |
| `-n` | `--line` | N | Yes | reference line number |
| `-a` | `--after` | — | No | insert after line N (default: before) |
| `-t` | `--text` | TEXT | Yes | text to insert |
| — | — | FILE | Yes | file to edit |

**Example — insert before line 2:**
```sh
./aishell edit-insert-line -n 2 -t "Inserted before line 2" test.txt
./aishell edit-show test.txt
   1  Hello world
   2  Inserted before line 2
   3  This is a test file
   4  Line three
```

**Example — insert after line 2:**
```sh
./aishell edit-insert-line -n 2 -a -t "Inserted after line 2" test.txt
./aishell edit-show test.txt
   1  Hello world
   2  This is a test file
   3  Inserted after line 2
   4  Line three
```

**Usage:**
```
Usage: edit-insert-line [-h] [--help-json] -n N [-a] -t TEXT FILE
  insert a line into a file at a given position
```

---

### Command 4: `edit-delete-line`

**Purpose:** Delete one or more consecutive lines by line number. With `-e`, deletes a range (N through E inclusive). All subsequent lines shift up.

**Source:** `cmd_edit_delete_line.c`

**Options:**
| Flag | Long | Arg | Required | Description |
|------|------|-----|----------|-------------|
| `-h` | `--help` | — | No | show help |
| — | `--help-json` | — | No | machine-readable help |
| `-n` | `--line` | N | Yes | first line to delete |
| `-e` | `--end` | E | No | last line (default: same as -n) |
| — | — | FILE | Yes | file to edit |

**Example — delete single line:**
```sh
./aishell edit-delete-line -n 2 test.txt
./aishell edit-show test.txt
   1  Hello world
   2  Line three
   3  Another line
```

**Example — delete a range (lines 2 through 3):**
```sh
./aishell edit-delete-line -n 2 -e 3 test.txt
./aishell edit-show test.txt
   1  Hello world
   2  Another line
```

**Usage:**
```
Usage: edit-delete-line [-h] [--help-json] -n N [-e E] FILE
  delete one or more lines from a file by line number
```

---

### Command 5: `edit-replace`

**Purpose:** Search every line of a file for a POSIX extended regex pattern and replace matches with a literal string. By default replaces the first match per line; `-g` replaces all. `-i` enables case-insensitive matching. The file is updated atomically.

**Source:** `cmd_edit_replace.c`

**Options:**
| Flag | Long | Arg | Required | Description |
|------|------|-----|----------|-------------|
| `-h` | `--help` | — | No | show help |
| — | `--help-json` | — | No | machine-readable help |
| `-p` | `--pattern` | PATTERN | Yes | POSIX extended regex |
| `-r` | `--replacement` | TEXT | Yes | replacement string |
| `-g` | `--global` | — | No | replace all matches per line |
| `-i` | `--ignore-case` | — | No | case-insensitive matching |
| — | — | FILE | Yes | file to edit |

**Internal replace engine:**
```c
static char *replace_in_line(regex_t *re, const char *line,
                              const char *repl, int global) {
    // Dynamically growing buffer
    // Walks the line with regexec() in a loop
    // Copies prefix → replacement → suffix
    // Handles zero-length matches to avoid infinite loop
    // Returns heap-allocated result
}
```
Used with `REG_EXTENDED` always, `REG_ICASE` optionally.

**Example — first match per line:**
```sh
./aishell edit-show test.txt
   1  Hello world hello
   2  HELLO there

./aishell edit-replace -p "hello" -r "HI" test.txt
./aishell edit-show test.txt
   1  Hello world HI
   2  HELLO there
```

**Example — global replace (all matches per line):**
```sh
./aishell edit-replace -p "hello" -r "HI" -g test.txt
./aishell edit-show test.txt
   1  Hello world HI
   2  HELLO there
```

**Example — case-insensitive + global:**
```sh
./aishell edit-replace -p "hello" -r "HI" -g -i test.txt
./aishell edit-show test.txt
   1  HI world HI
   2  HI there
```

**Usage:**
```
Usage: edit-replace [-h] [--help-json] -p PATTERN -r TEXT [-g] [-i] FILE
  find and replace text in a file using a regex pattern
```

---

### Files created for editor track

| File | Lines | Role |
|------|-------|------|
| `edit_utils.h` | 26 | Shared library interface |
| `edit_utils.c` | 65 | Shared library implementation |
| `cmd_edit_show.c` | ~130 | `edit-show` command |
| `cmd_edit_replace_line.c` | ~130 | `edit-replace-line` command |
| `cmd_edit_insert_line.c` | ~160 | `edit-insert-line` command |
| `cmd_edit_delete_line.c` | ~140 | `edit-delete-line` command |
| `cmd_edit_replace.c` | ~220 | `edit-replace` command |

**Registered in `aishell_main.c`:**
```c
extern void register_edit_replace_line_command(void);
extern void register_edit_insert_line_command(void);
extern void register_edit_delete_line_command(void);
extern void register_edit_replace_command(void);
extern void register_edit_show_command(void);
```
All five called in `register_all_commands()`.

---

### Editor command quick-reference

```sh
# View file (whole)
./aishell edit-show test.txt

# View file (lines 3 to 7)
./aishell edit-show -n 3 -e 7 test.txt

# Replace line 4 entirely
./aishell edit-replace-line -n 4 -t "new content" test.txt

# Insert a line before line 2
./aishell edit-insert-line -n 2 -t "new line" test.txt

# Insert a line after line 2
./aishell edit-insert-line -n 2 -a -t "new line" test.txt

# Delete line 5
./aishell edit-delete-line -n 5 test.txt

# Delete lines 5 through 8
./aishell edit-delete-line -n 5 -e 8 test.txt

# Replace first match per line
./aishell edit-replace -p "foo" -r "bar" test.txt

# Replace all matches per line, case-insensitive
./aishell edit-replace -p "foo" -r "bar" -g -i test.txt
```

---

## Track 2 — Package Management System

### Architecture Overview

```
pkg build  →  .tar.gz archive (with pkg.json inside)
                    │
pkg install  ←──────┘   reads pkg.json, extracts to ~/.mysh/pkgs/<name>-<ver>/
                              symlinks executables → ~/.mysh/bin/
                              records → ~/.mysh/pkgdb.txt

pkg list    reads pkgdb.txt → formatted table
pkg remove  reads pkgdb.txt → unlinks, rm -rf, rewrites db

                    ┌──────────────── Node.js Registry ───────────────────┐
pkg check-update    │  curl GET /packages/<name>  →  latestVersion        │
pkg upgrade         │  curl GET /packages/<name>  →  downloadUrl          │
                    └──────────────────────────────────────────────────────┘
```

---

### `pkg.json` — Package Metadata Format

Every package ships with a `pkg.json` at the root of the source directory.

**`week3/pkg.json`:**
```json
{
  "name": "aishell",
  "version": "2.0",
  "description": "BusyBox-style shell with 32 built-in commands",
  "files": ["aishell"],
  "docs": ["README.md"]
}
```

| Field | Purpose |
|-------|---------|
| `name` | Package identifier, used as install directory name and symlink name |
| `version` | Dot-separated version string, compared numerically during `check-update` |
| `files` | Executable names to symlink into `~/.mysh/bin/` |
| `docs` | Documentation files (informational) |

`pkg.json` is discovered at depth 0 then depth 1 (so `pkg build .` and `pkg build src/` both work).

---

### `pkg build`

**Command:** `pkg build <src-dir> <output.tar.gz>`

Creates a distributable archive from a source directory, reading `pkg.json` for metadata.

```sh
gcc -Wall -Wextra -std=c11 -o pkg pkg.c
./pkg build . ~/aishell-2.0.tar.gz
```
```
pkg: building from /home/jagan/aishell/week3
pkg: found pkg.json → aishell 2.0
pkg: archive → /home/jagan/aishell-2.0.tar.gz
```

**Internal tar command:**
```c
char *tar_args[] = {
    "tar",
    "--exclude=.git",              // prevent git objects
    "--exclude=aishell-2.0.tar.gz",// prevent self-include
    "-czf", out,
    "-C", src_dir, ".",
    NULL
};
unlink(out);   // remove stale file before writing
run_command(tar_args);
```

**Verify archive contents:**
```sh
tar -tzf ~/aishell-2.0.tar.gz | head -8
```
```
./
./pkg.json
./pkg.c
./aishell_main.c
./aishell
./Makefile
./README.md
```

---

### `pkg install`

**Command:** `pkg install <archive.tar.gz>`

Extracts the package, creates symlinks, and records in the local database.

```sh
./pkg install ~/aishell-2.0.tar.gz
```
```
pkg: installing aishell-2.0
pkg:   linked /home/jagan/.mysh/bin/aishell -> /home/jagan/.mysh/pkgs/aishell-2.0/aishell
pkg: aishell-2.0 installed successfully
```

**Install layout:**
```
~/.mysh/
├── bin/
│   └── aishell  →  ../pkgs/aishell-2.0/aishell   (symlink)
├── pkgs/
│   └── aishell-2.0/
│       ├── pkg.json
│       ├── aishell
│       └── README.md
└── pkgdb.txt                     ← "aishell 2.0"
```

**Two-pass extraction (chicken-and-egg solution):**

Install needs `name` and `version` from `pkg.json` before knowing the final install path:
```
Step 1: extract to /tmp/pkg_XXXXXX/    (mkdtemp)
Step 2: read /tmp/pkg_XXXXXX/pkg.json  (get name, version)
Step 3: extract to ~/.mysh/pkgs/<name>-<version>/
Step 4: symlink each file[] entry to ~/.mysh/bin/
Step 5: append "name version" to pkgdb.txt
```

`--strip-components=1` used at both steps to normalize `./` and `<name>/` archive roots.

---

### `pkg list`

**Command:** `pkg list`

Reads `~/.mysh/pkgdb.txt` and prints a formatted table.

```sh
./pkg list
```
```
NAME                     VERSION
------------------------ -------
aishell                  2.0
```

After installing multiple packages:
```
NAME                     VERSION
------------------------ -------
aishell                  2.0
hello                    1.0.0
wcplus                   2.1.0
```

---

### `pkg remove`

**Command:** `pkg remove <name>`

Looks up the installed version, removes symlinks, deletes the package directory, and removes the database entry.

```sh
./pkg remove aishell
```
```
pkg:   removed /home/jagan/.mysh/bin/aishell
pkg: aishell-2.0 removed
```

**Steps:**
1. `pkgdb_lookup("aishell")` → finds `"2.0"`
2. Reads `~/.mysh/pkgs/aishell-2.0/pkg.json` for `files[]`
3. `unlink()` each symlink in `~/.mysh/bin/`
4. `rm -rf ~/.mysh/pkgs/aishell-2.0/`
5. Rewrites `pkgdb.txt` without that entry

---

### Registry Server (Node.js + Express)

**File:** `week3/registry/server.js`

Minimal HTTP registry — an information broker for available packages and download URLs.

**Setup:**
```sh
cd ~/aishell/week3/registry
npm install       # first time only
node server.js
```
```
aishell registry  →  http://localhost:3000
  GET /packages
  GET /packages/:name
  GET /files/<archive>.tar.gz
```

**In-memory package list:**
```js
const packages = [
  { name: 'hello',   latestVersion: '1.0.0',
    downloadUrl: 'http://localhost:3000/files/hello-1.0.0.tar.gz' },
  { name: 'wcplus',  latestVersion: '2.1.0',
    downloadUrl: 'http://localhost:3000/files/wcplus-2.1.0.tar.gz' },
  { name: 'aishell', latestVersion: '2.0',
    downloadUrl: 'http://localhost:3000/files/aishell-2.0.tar.gz' },
];
```

**Test with curl:**
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

**Important:** Copying a `.tar.gz` into `registry/files/` does NOT auto-register it. Two steps always required:
1. Place archive in `registry/files/`
2. Add entry to `packages[]` in `server.js` + restart the server

---

### `pkg check-update`

**Command:** `pkg check-update <name>`

Queries the registry for the latest version and compares it against the locally installed version.

```sh
./pkg check-update aishell    # when update available
```
```
Update available: aishell  1.0 -> 2.0
  Run: pkg upgrade aishell
```

```sh
./pkg check-update aishell    # when up to date
```
```
aishell 2.0 is up to date
```

**Implementation — pipe-based stdout capture:**

`curl` is run as a subprocess; its stdout is captured via `pipe()` + `fork()` + `dup2()`:
```c
static int capture_command(char *const args[], char *buf, size_t bufsz) {
    int pipefd[2];
    pipe(pipefd);
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);    // child stdout → pipe
        close(pipefd[1]);
        int dn = open("/dev/null", O_WRONLY);
        dup2(dn, STDERR_FILENO);           // silence curl noise
        execvp(args[0], args);
        _exit(127);
    }
    close(pipefd[1]);
    // read loop collects output
    // waitpid returns exit code
}
```

**Version comparison — numeric, not lexicographic:**

String comparison gives wrong results: `"10" < "2"` alphabetically but `10 > 2` numerically.
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

---

### `pkg upgrade`

**Command:** `pkg upgrade <name>`

Downloads the latest version from the registry, removes the old install, and installs the new one.

**Full test sequence:**

Step 1 — install old version:
```sh
./pkg install ~/aishell-1.0.tar.gz
./pkg list
```
```
NAME                     VERSION
------------------------ -------
aishell                  1.0
```

Step 2 — check for update:
```sh
./pkg check-update aishell
```
```
Update available: aishell  1.0 -> 2.0
  Run: pkg upgrade aishell
```

Step 3 — upgrade:
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

Step 4 — verify:
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

**Upgrade internal flow:**
```
pkg upgrade aishell
    │
    ├─ curl registry/packages/aishell → latestVersion, downloadUrl
    ├─ pkgdb_lookup → installed version
    ├─ version_cmp(latest, installed) > 0  → proceed
    │
    ├─ mkdtemp /tmp/pkg_up_XXXXXX/
    ├─ curl -o /tmp/.../download.tar.gz  <downloadUrl>
    ├─ cmd_remove("aishell")      → clean old install
    ├─ cmd_install(tmp_tarball)   → extract, symlink, pkgdb update
    └─ rm -rf /tmp/pkg_up_XXXXXX/
```

---

### Shell Integration

Two changes to `aishell_main.c` to connect `pkg` with the REPL:

**1. PATH augmentation at startup (line 475 in main):**
```c
prepend_mysh_bin_to_path();   // makes ~/.mysh/bin visible to execvp
```
```c
static void prepend_mysh_bin_to_path(void) {
    const char *home = getenv("HOME");
    if (!home) return;
    char mysh_bin[512];
    snprintf(mysh_bin, sizeof(mysh_bin), "%s/.mysh/bin", home);
    const char *old = getenv("PATH");
    char new_path[4096];
    snprintf(new_path, sizeof(new_path), "%s:%s", mysh_bin, old ? old : "");
    setenv("PATH", new_path, 1);
}
```

**2. `pkg` dispatch in `execute_command()`:**
```c
const cmd_spec_t *spec = find_command(cmdname);
if (spec) {
    ret = spec->run(cmd->argc, cmd->argv);     // built-in
} else if (strcmp(cmdname, "pkg") == 0) {
    ret = exec_external(cmd);                  // fork+execvp pkg binary
} else {
    fprintf(stderr, "aishell: %s: command not found\n", cmd->argv[0]);
    ret = 127;
}
```

**Demo inside the shell REPL:**
```sh
make && ./aishell
```
```
jshell% pkg list
NAME                     VERSION
------------------------ -------
aishell                  2.0

jshell% pkg check-update aishell
aishell 2.0 is up to date

jshell% edit-show test.txt
   1  Hello world
   2  This is a test file
   3  Line three

jshell% edit-replace-line -n 2 -t "Updated from shell" test.txt
jshell% edit-show test.txt
   1  Hello world
   2  Updated from shell
   3  Line three

jshell% exit
```

---

## Problems Encountered and Solutions

### 1. `.git/` objects included in archive
- **Symptom:** `tar -tzf` showed `.git/objects/pack/...` in the listing
- **Fix:** `--exclude=.git` added to tar args

### 2. "Permission denied" writing tarball
- **Symptom:** EACCES on `/tmp/aishell-1.0.tar.gz` owned by root from a prior run
- **Fix:** `unlink(out)` before tar call — removes stale file, harmless if missing

### 3. GCC `-Wformat-truncation` warnings
- **Symptom:** Many `output may be truncated` warnings on `snprintf("%s/suffix", base)`
- **Fix:** Defined separate size constants per semantic level:
  ```c
  #define PKG_BASE_LEN  256   // ~/.mysh
  #define PKG_PATH_MAX  512   // single segment
  #define PKG_FULL_MAX 1024   // two-segment paths
  ```

### 4. `realpath()` implicit declaration
- **Symptom:** Warning with `_POSIX_C_SOURCE 200809L`
- **Fix:** Switch to `#define _GNU_SOURCE`

### 5. Archive root normalization
- **Symptom:** Files sometimes landed in `aishell-2.0/aishell/` instead of `aishell-2.0/`
- **Fix:** `--strip-components=1` on all extractions normalizes `./` and `<name>/` roots

### 6. Installed version not updating after upgrade
- **Symptom:** `pkg list` showed `1.0` after upgrading to `2.0`
- **Root cause:** `pkg.json` inside the archive still had `"version": "1.0"` — archive was built before updating the metadata
- **Fix:** Always update `pkg.json` before `pkg build`, then copy archive to `registry/files/` and restart server

### 7. Registry appeared to "do nothing"
- **Symptom:** Copied `.tar.gz` to `registry/files/`, nothing changed in `pkg check-update`
- **Root cause:** Registry is a pull-based information broker; files are not auto-discovered
- **Fix (process):** File + `packages[]` entry + server restart → then `pkg check-update` fetches fresh data

### 8. Infinite loop risk in `edit-replace` for zero-length regex matches
- **Pattern:** A regex like `a*` can match zero characters, causing `regexec` to match at the same position forever
- **Fix:**
  ```c
  if (m.rm_eo == m.rm_so) {
      // zero-length match: advance one char to break the loop
      buf[bpos++] = *p++;
  }
  ```

---

## Architecture Decisions

| Decision | Rationale |
|----------|-----------|
| Shared `edit_utils` library | Five commands share identical read/write logic — avoids duplication and keeps atomic-write behavior consistent |
| `rename()` for atomic writes | POSIX rename is atomic — original file is never corrupted if write fails mid-way |
| POSIX extended regex in `edit-replace` | `REG_EXTENDED` gives full ERE syntax; standard library, no dependency |
| Dynamic line buffer growth | Files of any size are handled; no fixed line count limit |
| Plain-text `pkgdb.txt` | Zero dependencies, human-readable, sufficient for the scale |
| Minimal JSON parser (strstr-based) | No external library needed to read 5-field `pkg.json` |
| `curl` via subprocess for HTTP | Avoids libcurl linking; `capture_command()` reads stdout via pipe |
| `--strip-components=1` always | Normalizes archive roots regardless of how the tarball was built |
| Two-pass extraction in `pkg install` | Metadata must be read before the install path is known |
| `unlink()` before `tar` write | Handles stale files from prior runs or different users |
| `#define _GNU_SOURCE` | Required for `realpath()` on glibc |
| External `pkg` binary (not built-in) | Keeps pkg separate from the shell — can be updated without recompiling aishell |
| `~/.mysh/bin` prepended to PATH | Installed packages are available as shell commands without user configuration |

---

## Files Created / Modified

| File | Action | Lines | Description |
|------|--------|-------|-------------|
| `edit_utils.h` | Created | 26 | Shared editor library interface |
| `edit_utils.c` | Created | 65 | Atomic read/write, dynamic buffer, newline helper |
| `cmd_edit_show.c` | Created | ~130 | View file with line numbers, range support |
| `cmd_edit_replace_line.c` | Created | ~130 | Replace entire line by number |
| `cmd_edit_insert_line.c` | Created | ~160 | Insert line before/after N |
| `cmd_edit_delete_line.c` | Created | ~140 | Delete line or range |
| `cmd_edit_replace.c` | Created | ~220 | Regex find-and-replace, global, case-insensitive |
| `pkg.c` | Created | 838 | Full package manager — 6 subcommands |
| `pkg.json` | Created | 8 | aishell package descriptor |
| `aishell_main.c` | Modified | — | PATH augmentation + pkg dispatch + sys/wait.h |
| `registry/server.js` | Created | 82 | Express HTTP registry server |
| `registry/package.json` | Created | — | npm metadata, express dependency |
| `.gitignore` | Modified | — | Exclude `registry/node_modules/`, `registry/files/*.tar.gz` |

---

## Command Summary

### Editor Commands

| Command | Key Options | Example |
|---------|------------|---------|
| `edit-show FILE` | `-n N` start, `-e E` end | `edit-show -n 2 -e 5 file.txt` |
| `edit-replace-line -n N -t TEXT FILE` | `-n` line, `-t` text | `edit-replace-line -n 3 -t "new" file.txt` |
| `edit-insert-line -n N -t TEXT FILE` | `-a` insert after | `edit-insert-line -n 2 -a -t "new" file.txt` |
| `edit-delete-line -n N FILE` | `-e E` end of range | `edit-delete-line -n 2 -e 4 file.txt` |
| `edit-replace -p PAT -r TEXT FILE` | `-g` global, `-i` ignore-case | `edit-replace -p "foo" -r "bar" -g -i file.txt` |

### Package Manager Commands

| Command | Description | Example |
|---------|-------------|---------|
| `pkg build <dir> <out.tar.gz>` | Package source into archive | `./pkg build . ~/aishell-2.0.tar.gz` |
| `pkg install <archive>` | Install from local archive | `./pkg install ~/aishell-2.0.tar.gz` |
| `pkg list` | Show installed packages | `./pkg list` |
| `pkg remove <name>` | Uninstall package | `./pkg remove aishell` |
| `pkg check-update <name>` | Check registry for newer version | `./pkg check-update aishell` |
| `pkg upgrade <name>` | Download and install latest | `./pkg upgrade aishell` |

---

## Week 4 Outcome

aishell now ships with:

- **5 text editor built-in commands** capable of viewing, modifying, inserting, deleting, and regex-replacing content in any text file — entirely from the shell prompt, backed by a shared atomic-write library
- **A full package lifecycle manager** (`pkg`) that can build, distribute, install, track, remove, and upgrade packages using a local database and an HTTP registry
- **A live registry server** that exposes package metadata and serves tarballs over HTTP, acting as the central source of truth for available packages
- **Shell integration** that makes installed packages available as first-class commands inside the aishell REPL with no manual PATH setup

Total new source files this week: **13**
Total new lines of C: **~1700**
External dependencies added: **express** (registry server only)

---

*Generated: 2026-05-13 | aishell week4 | AI-assisted development log*
