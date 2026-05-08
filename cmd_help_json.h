#ifndef CMD_HELP_JSON_H
#define CMD_HELP_JSON_H

#include <stdio.h>
#include "cmd_spec.h"

void print_help_json(const cmd_spec_t *spec, void **argtable, FILE *out);

#endif
