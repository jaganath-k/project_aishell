#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

/* ── Alias table functions defined in aishell_main.c ─────────────────── */
extern void        alias_set(const char *name, const char *value);
extern const char *alias_get(const char *name);
extern void        alias_del(const char *name);
extern void        alias_print_all(FILE *out);

/* ═══════════════════════════════════════════════════════════════════════
 * alias
 * ═══════════════════════════════════════════════════════════════════════ */

static void build_alias_argtable(arg_lit_t **help,
                                  arg_lit_t **help_json,
                                  arg_str_t **defs,
                                  arg_end_t **end,
                                  void      ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json", "print machine-readable help as JSON");
    *defs      = arg_strn(NULL, NULL, "NAME[=VALUE]", 0, 64,
                          "define alias (NAME=VALUE) or print alias (NAME)");
    *end       = arg_end(20);

    static void *argtable[5];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *defs;
    argtable[3] = *end;
    argtable[4] = NULL;
    *argtable_out = argtable;
}

void alias_print_usage(FILE *out);
extern cmd_spec_t cmd_alias_spec;

int alias_run(int argc, char **argv)
{
    arg_lit_t *help, *help_json;
    arg_str_t *defs;
    arg_end_t *end;
    void     **argtable;

    build_alias_argtable(&help, &help_json, &defs, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 4);
        alias_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_alias_spec, argtable, stdout);
        arg_freetable(argtable, 4);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "alias");
        arg_freetable(argtable, 4);
        alias_print_usage(stdout);
        return 1;
    }

    int ndefs = defs->count;
    /* Copy strings before freeing argtable */
    char *args[64];
    for (int i = 0; i < ndefs && i < 64; i++)
        args[i] = strdup(defs->sval[i]);
    arg_freetable(argtable, 4);

    if (ndefs == 0) {
        alias_print_all(stdout);
        return 0;
    }

    int rc = 0;
    for (int i = 0; i < ndefs; i++) {
        char *eq = strchr(args[i], '=');
        if (eq) {
            /* NAME=VALUE — define alias */
            *eq = '\0';
            alias_set(args[i], eq + 1);
        } else {
            /* NAME — print that alias */
            const char *val = alias_get(args[i]);
            if (val)
                printf("alias %s='%s'\n", args[i], val);
            else {
                fprintf(stderr, "alias: %s: not found\n", args[i]);
                rc = 1;
            }
        }
        free(args[i]);
    }
    return rc;
}

void alias_print_usage(FILE *out)
{
    arg_lit_t *help, *help_json;
    arg_str_t *defs;
    arg_end_t *end;
    void     **argtable;

    build_alias_argtable(&help, &help_json, &defs, &end, &argtable);
    fprintf(out, "Usage: alias ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nDefine or display aliases.\n");
    fprintf(out, "  alias              print all defined aliases\n");
    fprintf(out, "  alias NAME=VALUE   define an alias\n");
    fprintf(out, "  alias NAME         print one alias\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-28s %s\n");
    arg_freetable(argtable, 4);
}

cmd_spec_t cmd_alias_spec = {
    .name      = "alias",
    .summary   = "define or display aliases",
    .long_help = "With no arguments, print all defined aliases. "
                 "With NAME=VALUE, define a new alias. "
                 "With NAME alone, print the alias for that name. "
                 "Aliases are expanded when they appear as the first word of a command.",
    .run         = alias_run,
    .print_usage = alias_print_usage,
};

void register_alias_command(void) {
    register_command(&cmd_alias_spec);
}

/* ═══════════════════════════════════════════════════════════════════════
 * unalias
 * ═══════════════════════════════════════════════════════════════════════ */

static void build_unalias_argtable(arg_lit_t **help,
                                    arg_lit_t **help_json,
                                    arg_str_t **names,
                                    arg_end_t **end,
                                    void      ***argtable_out)
{
    *help      = arg_lit0("h", "help",      "show help and exit");
    *help_json = arg_lit0(NULL, "help-json", "print machine-readable help as JSON");
    *names     = arg_strn(NULL, NULL, "NAME", 0, 64, "alias name(s) to remove");
    *end       = arg_end(20);

    static void *argtable[5];
    argtable[0] = *help;
    argtable[1] = *help_json;
    argtable[2] = *names;
    argtable[3] = *end;
    argtable[4] = NULL;
    *argtable_out = argtable;
}

void unalias_print_usage(FILE *out);
extern cmd_spec_t cmd_unalias_spec;

int unalias_run(int argc, char **argv)
{
    arg_lit_t *help, *help_json;
    arg_str_t *names;
    arg_end_t *end;
    void     **argtable;

    build_unalias_argtable(&help, &help_json, &names, &end, &argtable);
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count > 0) {
        arg_freetable(argtable, 4);
        unalias_print_usage(stdout);
        return 0;
    }
    if (help_json->count > 0) {
        print_help_json(&cmd_unalias_spec, argtable, stdout);
        arg_freetable(argtable, 4);
        return 0;
    }
    if (nerrors > 0) {
        arg_print_errors(stdout, end, "unalias");
        arg_freetable(argtable, 4);
        unalias_print_usage(stdout);
        return 1;
    }

    int nnames = names->count;
    if (nnames == 0) {
        fprintf(stderr, "unalias: usage: unalias NAME [NAME...]\n");
        arg_freetable(argtable, 4);
        return 1;
    }

    int rc = 0;
    for (int i = 0; i < nnames; i++) {
        if (!alias_get(names->sval[i])) {
            fprintf(stderr, "unalias: %s: not found\n", names->sval[i]);
            rc = 1;
        } else {
            alias_del(names->sval[i]);
        }
    }
    arg_freetable(argtable, 4);
    return rc;
}

void unalias_print_usage(FILE *out)
{
    arg_lit_t *help, *help_json;
    arg_str_t *names;
    arg_end_t *end;
    void     **argtable;

    build_unalias_argtable(&help, &help_json, &names, &end, &argtable);
    fprintf(out, "Usage: unalias ");
    arg_print_syntax(out, argtable, "\n");
    fprintf(out, "\nRemove alias definitions.\n");
    fprintf(out, "\nOptions:\n");
    arg_print_glossary(out, argtable, "  %-28s %s\n");
    arg_freetable(argtable, 4);
}

cmd_spec_t cmd_unalias_spec = {
    .name      = "unalias",
    .summary   = "remove alias definitions",
    .long_help = "Remove each NAME from the list of defined aliases.",
    .run         = unalias_run,
    .print_usage = unalias_print_usage,
};

void register_unalias_command(void) {
    register_command(&cmd_unalias_spec);
}
