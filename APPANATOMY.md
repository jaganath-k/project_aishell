# APP ANATOMY – Instructions for AI Refactoring

This file describes the **standard anatomy of a command-line app** in this course and how an AI assistant should **refactor existing commands** (e.g. `ls`, `wc`, `cat`, `pkg`) to match that anatomy.

Scope: applies to all command modules that are part of the custom shell and its package ecosystem.

The goal is:

- One **uniform structure** per command.
- Single source of truth for:
  - CLI options and arguments (`argtable3`).
  - Help / documentation text.
  - Shell built‑in integration.
  - Packaging metadata (`pkg.json`, docs in the package).

An AI agent using this file should follow these rules when refactoring existing code.

---

## Target structure: `cmd_spec_t` and command module

Each command is implemented as a **module** with:

1. A command specification struct `cmd_spec_t`.
2. A `run` function that implements the command.
3. A `print_usage` function that prints help based on `argtable3`.
4. (Optional) A tiny `main()` wrapper so the same logic can be built as a standalone binary.

The basic types are:

```c
typedef struct cmd_spec {
    const char *name;        // command name, e.g. "ls"
    const char *summary;     // one‑line description
    const char *long_help;   // longer description / Markdown (may be NULL)

    // Main entrypoint: parses args with argtable3 and runs the command.
    int (*run)(int argc, char **argv);

    // Prints usage and option help (using argtable3) to the given stream.
    void (*print_usage)(FILE *out);
} cmd_spec_t;
```

Each command module must define **exactly one** instance of this struct, e.g.:

```c
extern cmd_spec_t cmd_ls_spec;
```

and in its `.c` file:

```c
cmd_spec_t cmd_ls_spec = {
    .name        = "ls",
    .summary     = "list directory contents",
    .long_help   = "List information about the FILEs (the current directory by default).",
    .run         = ls_run,
    .print_usage = ls_print_usage,
};
```

---

## Registry and shell integration

The shell exposes a simple command registry:

```c
void register_command(const cmd_spec_t *spec);
const cmd_spec_t *find_command(const char *name);
void for_each_command(void (*cb)(const cmd_spec_t *spec, void *userdata), void *userdata);
```

There is a helper function per command:

```c
// Implemented in each command module
void register_ls_command(void) {
    register_command(&cmd_ls_spec);
}
```

The shell initialization calls:

```c
void register_all_builtin_commands(void) {
    register_ls_command();
    register_wc_command();
    register_cat_command();
    register_pkg_command();
    // ...
}
```

The shell’s `help` built‑in can use `for_each_command` and `spec->print_usage` to list commands and show detailed help.

When refactoring, the AI should:

- Introduce a `cmd_<name>_spec` and a `register_<name>_command()` function for each command.
- Ensure the shell uses `find_command()` to dispatch built‑ins instead of hard‑coding them.

---

## argtable3 usage and CLI behavior

Every command’s `run` and `print_usage` functions must use **`argtable3`** as the single source of truth for CLI syntax.

Pattern:

```c
static void build_ls_argtable(struct arg_lit **help,
                              struct arg_lit **all,
                              struct arg_file **paths,
                              struct arg_end **end,
                              void ***argtable_out)
{
    *help  = arg_lit0("h", "help", "show help and exit");
    *all   = arg_lit0("a", "all",  "do not ignore entries starting with .");
    *paths = arg_file0(NULL, NULL, "[PATH...]", "directories or files to list");
    *end   = arg_end(20);

    static void *argtable[5];
    argtable[0] = *help;
    argtable[1] = *all;
    argtable[2] = *paths;
    argtable[3] = *end;
    argtable[4] = NULL;

    *argtable_out = argtable;
}
```

`run` should:

```c
int ls_run(int argc, char **argv)
{
    struct arg_lit  *help;
    struct arg_lit  *all;
    struct arg_file *paths;
    struct arg_end  *end;
    void           **argtable;

    build_ls_argtable(&help, &all, &paths, &end, &argtable);

    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        ls_print_usage(stdout);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "ls");
        ls_print_usage(stdout);
        return 1;
    }

    // Existing ls logic goes here, using values from `all`, `paths`, etc.
}
```

`print_usage` should:

```c
void ls_print_usage(FILE *out)
{
    struct arg_lit  *help;
    struct arg_lit  *all;
    struct arg_file *paths;
    struct arg_end  *end;
    void           **argtable;

    build_ls_argtable(&help, &all, &paths, &end, &argtable);

    fprintf(out, "Usage: ls ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-20s %s\n");
}
```

When refactoring an existing command that already parses `argv` manually, the AI should:

- Map the existing options and positional arguments into `argtable3` definitions.
- Preserve semantics (behavior, defaults, error messages) as much as reasonable.
- Ensure `--help` is implemented and prints usage.

---

## Documentation extraction and package metadata

The package manager (`pkg`) and packaging tools will **not parse C source code**. Instead, they will:

1. Either:
   - Call `spec->print_usage()` and capture the output into a documentation file, or
   - Run the standalone binary with a special flag (e.g. `--help-md` or `--help-json`) that prints machine-readable help.
2. Include these docs in the package’s `docs` section (e.g. `README.md`, `docs/<cmd>.md`).

When refactoring:

- Ensure `print_usage()` produces complete, clear help text.
- Optionally add a `--help-md` or `--help-json` mode to `run()` that prints Markdown/JSON; base this on the same `argtable3` definitions.
- Fill `summary` and `long_help` in `cmd_spec_t` with concise, human-readable descriptions. These are used for:
  - The shell’s `help` command.
  - The `description` field in `pkg.json`.

---

## Standalone binaries vs built-ins

The same command module should support two build targets:

1. **Built-in shell command**:
   - The module is compiled into a library or object file.
   - The shell binary links this library and calls `register_<name>_command()`.

2. **Standalone app**:
   - A tiny `main()` in a separate file uses the module:

   ```c
   int main(int argc, char **argv) {
       return cmd_ls_spec.run(argc, argv);
   }
   ```

When refactoring, the AI should:

- Extract the core logic from `main()` into `run()` and `print_usage()`.
- Create a new `main()` wrapper if a standalone binary is still desired.

---

## Refactoring guidelines for existing commands

When given an existing implementation (e.g. `ls.c`, `wc.c`, `cat.c`) the AI agent should:

1. **Identify the command name and behavior**:
   - What does the command do?
   - Which options and arguments does it support?

2. **Introduce the standard module structure**:
   - Create or modify a module file (e.g. `cmd_ls.c`) that defines:
     - `int ls_run(int argc, char **argv);`
     - `void ls_print_usage(FILE *out);`
     - `cmd_spec_t cmd_ls_spec;`
     - `void register_ls_command(void);`

3. **Move logic into `run()`**:
   - Move the body of `main()` (or equivalent) into `ls_run()`.
   - Replace manual `argv` parsing with an `argtable3`-based implementation.
   - Keep the user-visible behavior (flags, exit codes, outputs) as close as possible.

4. **Implement `print_usage()` using `argtable3`**:
   - Ensure it prints:
     - A usage line.
     - Option glossary.
     - Any positional argument description.

5. **Fill in `cmd_spec_t` metadata**:
   - `name`: the executable/command name.
   - `summary`: a one-line description.
   - `long_help`: a paragraph or Markdown-style description.

6. **Add a small `main()` wrapper if needed**:
   - In a separate file (e.g. `ls_main.c`), create a `main()` that calls `cmd_ls_spec.run`.
   - Keep `main()` minimal; no logic beyond delegating to `run()`.

7. **Integrate with the shell registry**:
   - Ensure `register_ls_command()` calls `register_command(&cmd_ls_spec);`.
   - Confirm that the shell’s `help` command can expose `ls` via the registry.

8. **Do not introduce unnecessary dependencies or large restructurings**:
   - Use only `argtable3`, the C standard library, and existing project conventions.
   - Preserve the surrounding style (indentation, naming) of the codebase.

---

## Special case: `pkg` command

The `pkg` package manager is **also** a command following the same anatomy:

```c
int pkg_run(int argc, char **argv);
void pkg_print_usage(FILE *out);

cmd_spec_t cmd_pkg_spec = {
    .name        = "pkg",
    .summary     = "manage mysh packages",
    .long_help   = "Build, install, list, remove, and upgrade packages for the mysh shell.",
    .run         = pkg_run,
    .print_usage = pkg_print_usage,
};
```

`pkg` will additionally:

- Provide subcommands (e.g. `build`, `install`, `list`, `remove`, `check-update`, `upgrade`, `compile`).
- Use `argtable3` for its own CLI.
- Use the command registry and `print_usage` to generate documentation for packages.

When refactoring or extending `pkg`, follow the same anatomy and keep subcommand parsing clear and modular.


