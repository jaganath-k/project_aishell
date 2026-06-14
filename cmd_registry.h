#pragma once

#define REGISTRY_MAX_ENTRIES  64
#define REGISTRY_MAX_ALIASES  10
#define REGISTRY_MAX_ARGS      5

typedef struct {
    char id[64];
    char description[256];
    char command[512];
    char aliases[REGISTRY_MAX_ALIASES][128];
    int  alias_count;
    char category[32];
    int  requires_arg;
    char args[REGISTRY_MAX_ARGS][32];
    int  arg_count;
} RegistryEntry;

/* Load commands.json at startup. Returns number of entries loaded, -1 on error. */
int            registry_load(const char *json_path);

/* Keyword match: returns best entry or NULL if no match (needs 2+ word overlap). */
RegistryEntry *registry_lookup(const char *query);

/* Extract argument values from a natural-language query for the given entry.
 * extracted[i][256] receives the value for entry->args[i], or "" if not found.
 * Returns the number of args for which a value was successfully extracted.   */
int            extract_args_for_entry(RegistryEntry *entry, const char *query,
                                      char extracted[][256], int max_args);

/* Substitute {placeholder} tokens in entry->command with values extracted from
 * the natural-language query. Returns malloc'd string; caller must free().
 * Returns NULL and prints a usage hint if a required placeholder is missing. */
char          *registry_build_command(RegistryEntry *entry, const char *query);

/* Print all loaded registry entries to stdout. */
void           registry_list(void);

/* Iterate all loaded entries, calling cb(entry, userdata) for each. */
void           registry_for_each(void (*cb)(const RegistryEntry *e, void *ud),
                                 void *ud);

/* Return the number of entries currently loaded. */
int            registry_count(void);
