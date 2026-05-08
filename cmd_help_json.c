#include <stdio.h>
#include <argtable3.h>
#include "cmd_spec.h"
#include "cmd_help_json.h"

static void jstr(FILE *out, const char *s) {
    if (!s) { fprintf(out, "null"); return; }
    fputc('"', out);
    for (const char *p = s; *p; p++) {
        switch (*p) {
        case '"':  fprintf(out, "\\\""); break;
        case '\\': fprintf(out, "\\\\"); break;
        case '\n': fprintf(out, "\\n");  break;
        default:   fputc(*p, out);       break;
        }
    }
    fputc('"', out);
}

void print_help_json(const cmd_spec_t *spec, void **argtable, FILE *out) {
    fprintf(out, "{\n");
    fprintf(out, "  \"name\": ");        jstr(out, spec->name);
    fprintf(out, ",\n  \"summary\": ");  jstr(out, spec->summary);
    fprintf(out, ",\n  \"description\": "); jstr(out, spec->long_help);
    fprintf(out, ",\n  \"options\": [");

    int first = 1;
    for (int i = 0; argtable[i] != NULL; i++) {
        arg_hdr_t *hdr = (arg_hdr_t *)argtable[i];
        if (hdr->flag & ARG_TERMINATOR)
            break;

        if (!first) fprintf(out, ",");
        first = 0;
        fprintf(out, "\n    {\n");

        fprintf(out, "      \"short\": ");
        if (hdr->shortopts && hdr->shortopts[0])
            fprintf(out, "\"%c\"", hdr->shortopts[0]);
        else
            fprintf(out, "null");

        fprintf(out, ",\n      \"long\": ");
        if (hdr->longopts && hdr->longopts[0])
            jstr(out, hdr->longopts);
        else
            fprintf(out, "null");

        fprintf(out, ",\n      \"datatype\": ");
        jstr(out, (hdr->datatype && hdr->datatype[0]) ? hdr->datatype : NULL);

        fprintf(out, ",\n      \"description\": ");
        jstr(out, hdr->glossary);

        fprintf(out, ",\n      \"required\": %s",
                hdr->mincount > 0 ? "true" : "false");

        fprintf(out, "\n    }");
    }

    fprintf(out, "%s]\n}\n", first ? "" : "\n  ");
}
