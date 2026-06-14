#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

static void build_ln_argtable(arg_lit_t  **help,
                               arg_lit_t  **help_json,
                               arg_lit_t  **symbolic,
                               arg_lit_t  **force,
                               arg_lit_t  **verbose,
                               arg_file_t **files,
                               arg_end_t  **end,
                               void       ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json", "print machine-readable help as JSON");
    *symbolic  = arg_lit0("s", "symbolic",  "create a symbolic link instead of a hard link");
    *force     = arg_lit0("f", "force",     "remove destination if it already exists");
    *verbose   = arg_lit0("v", "verbose",   "print a message for each link created");
    *files     = arg_filen(NULL, NULL, "FILE", 2, 2, "TARGET and LINK_NAME (exactly 2)");
    *end       = arg_end(20);

    static void *argtable[8];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *symbolic;
    argtable[3] = *force;
    argtable[4] = *verbose;
    argtable[5] = *files;
    argtable[6] = *end;
    argtable[7] = NULL;
    *argtable_out = argtable;
}

void ln_print_usage(FILE *out);
extern cmd_spec_t cmd_ln_spec;

int ln_run(int argc, char **argv) {
    arg_lit_t  *help, *help_json, *symbolic, *force, *verbose;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_ln_argtable(&help, &help_json, &symbolic, &force, &verbose,
                      &files, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 7);
        ln_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_ln_spec, argtable, stdout);
        arg_freetable(argtable, 7);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "ln");
        arg_freetable(argtable, 7);
        ln_print_usage(stdout);
        return 1;
    }

    int do_sym     = (symbolic->count > 0);
    int do_force   = (force->count > 0);
    int do_verbose = (verbose->count > 0);
    const char *target    = files->filename[0];
    const char *link_name = files->filename[1];

    arg_freetable(argtable, 7);

    /* Remove existing destination if -f */
    if (do_force) {
        if (unlink(link_name) < 0 && errno != ENOENT) {
            fprintf(stderr, "ln: cannot remove '%s': %s\n", link_name, strerror(errno));
            return 1;
        }
    }

    /* Create the link */
    int ret = do_sym ? symlink(target, link_name) : link(target, link_name);
    if (ret < 0) {
        fprintf(stderr, "ln: failed to create %s link '%s' -> '%s': %s\n",
                do_sym ? "symbolic" : "hard", link_name, target, strerror(errno));
        return 1;
    }

    if (do_verbose)
        printf("'%s' -> '%s'\n", link_name, target);

    return 0;
}

void ln_print_usage(FILE *out) {
    arg_lit_t  *help, *help_json, *symbolic, *force, *verbose;
    arg_file_t *files;
    arg_end_t  *end;
    void      **argtable;

    build_ln_argtable(&help, &help_json, &symbolic, &force, &verbose,
                      &files, &end, &argtable);
    fprintf(out, "Usage: ln ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nCreate links between files.\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-28s %s\n");
    arg_freetable(argtable, 7);
}

cmd_spec_t cmd_ln_spec = {
    .name      = "ln",
    .summary   = "make links between files",
    .long_help = "Create a hard link (default) or symbolic link (-s) from TARGET to LINK_NAME. "
                 "Use -f to remove an existing destination before linking. "
                 "Use -v to print each link created.",
    .run         = ln_run,
    .print_usage = ln_print_usage,
};

void register_ln_command(void) {
    register_command(&cmd_ln_spec);
}
