#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <regex.h>
#include "cJSON.h"
#include "cmd_registry.h"

static RegistryEntry g_registry[REGISTRY_MAX_ENTRIES];
static int           g_registry_count = 0;

/* ── helpers ──────────────────────────────────────────────────────────────── */

static void str_lower(char *dst, const char *src, size_t max) {
    size_t i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = '\0';
}

/* Copy a JSON string field into dst (truncated to dstsz). */
static void copy_str(char *dst, size_t dstsz, const cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring)
        snprintf(dst, dstsz, "%s", item->valuestring);
}

/* ── registry_load ────────────────────────────────────────────────────────── */

int registry_load(const char *json_path) {
    FILE *f = fopen(json_path, "r");
    if (!f) {
        fprintf(stderr, "[registry] Warning: %s not found — @ will use MCP/Claude only.\n",
                json_path);
        return -1;
    }

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)fsize + 1);
    if (!buf) { fclose(f); return -1; }
    size_t nread = fread(buf, 1, (size_t)fsize, f);
    buf[nread] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        fprintf(stderr, "[registry] JSON parse error in %s\n", json_path);
        return -1;
    }

    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "commands");
    if (!cJSON_IsArray(arr)) {
        fprintf(stderr, "[registry] No 'commands' array in %s\n", json_path);
        cJSON_Delete(root);
        return -1;
    }

    g_registry_count = 0;
    cJSON *entry;
    cJSON_ArrayForEach(entry, arr) {
        if (g_registry_count >= REGISTRY_MAX_ENTRIES) break;
        RegistryEntry *e = &g_registry[g_registry_count];
        memset(e, 0, sizeof(*e));

        copy_str(e->id,          sizeof(e->id),          entry, "id");
        copy_str(e->description, sizeof(e->description), entry, "description");
        copy_str(e->command,     sizeof(e->command),     entry, "command");
        copy_str(e->category,    sizeof(e->category),    entry, "category");

        cJSON *req = cJSON_GetObjectItemCaseSensitive(entry, "requires_arg");
        e->requires_arg = cJSON_IsTrue(req) ? 1 : 0;

        /* aliases array */
        cJSON *aliases = cJSON_GetObjectItemCaseSensitive(entry, "aliases");
        if (cJSON_IsArray(aliases)) {
            cJSON *al;
            cJSON_ArrayForEach(al, aliases) {
                if (e->alias_count >= REGISTRY_MAX_ALIASES) break;
                if (cJSON_IsString(al) && al->valuestring)
                    snprintf(e->aliases[e->alias_count++],
                             sizeof(e->aliases[0]), "%s", al->valuestring);
            }
        }

        /* args array */
        cJSON *args = cJSON_GetObjectItemCaseSensitive(entry, "args");
        if (cJSON_IsArray(args)) {
            cJSON *ag;
            cJSON_ArrayForEach(ag, args) {
                if (e->arg_count >= REGISTRY_MAX_ARGS) break;
                if (cJSON_IsString(ag) && ag->valuestring)
                    snprintf(e->args[e->arg_count++],
                             sizeof(e->args[0]), "%s", ag->valuestring);
            }
        }

        g_registry_count++;
    }

    cJSON_Delete(root);
    return g_registry_count;
}

/* ── registry_lookup ──────────────────────────────────────────────────────── */

/* Tokenise src into words (space-delimited), store in words[], return count. */
static int tokenise_words(const char *src, char words[][64], int max_words) {
    char buf[512];
    str_lower(buf, src, sizeof(buf));
    int count = 0;
    char *tok = strtok(buf, " \t\n");
    while (tok && count < max_words) {
        /* Skip very short filler words */
        if (strlen(tok) > 1)
            snprintf(words[count++], 64, "%s", tok);
        tok = strtok(NULL, " \t\n");
    }
    return count;
}

/* Count how many query words appear in the alias string. */
static int alias_score(const char *alias, char words[][64], int nwords) {
    char al_low[256];
    str_lower(al_low, alias, sizeof(al_low));
    int score = 0;
    for (int i = 0; i < nwords; i++) {
        if (strstr(al_low, words[i]))
            score++;
    }
    return score;
}

RegistryEntry *registry_lookup(const char *query) {
    if (!query || g_registry_count == 0) return NULL;

    char words[32][64];
    int  nwords = tokenise_words(query, words, 32);
    if (nwords == 0) return NULL;

    RegistryEntry *best = NULL;
    int best_score = 1;   /* require at least 2 matching words */

    for (int i = 0; i < g_registry_count; i++) {
        RegistryEntry *e = &g_registry[i];
        for (int j = 0; j < e->alias_count; j++) {
            int sc = alias_score(e->aliases[j], words, nwords);
            if (sc > best_score) {
                best_score = sc;
                best = e;
            }
        }
        /* Also score against description for extra coverage */
        int desc_sc = alias_score(e->description, words, nwords);
        if (desc_sc > best_score) {
            best_score = desc_sc;
            best = e;
        }
    }

    return best;
}

/* ── registry_build_command ───────────────────────────────────────────────── */

/* Extract a path token (starts with / ~ or .) from query. */
static int extract_path(const char *query, char *out, size_t outsz) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", query);
    char *tok = strtok(buf, " \t\n");
    while (tok) {
        if (tok[0] == '/' || tok[0] == '~' || tok[0] == '.') {
            snprintf(out, outsz, "%s", tok);
            return 1;
        }
        tok = strtok(NULL, " \t\n");
    }
    return 0;
}

/* Extract first integer anywhere in query (raw fallback). */
static int extract_number(const char *query, char *out, size_t outsz) {
    const char *p = query;
    while (*p) {
        if (isdigit((unsigned char)*p)) {
            const char *start = p;
            while (isdigit((unsigned char)*p)) p++;
            size_t len = (size_t)(p - start);
            if (len >= outsz) len = outsz - 1;
            memcpy(out, start, len);
            out[len] = '\0';
            return 1;
        }
        p++;
    }
    return 0;
}

/* Use regex to extract a number with temporal context:
 *   "than [0-9]+"   → "older than 30 days"
 *   "[0-9]+ days?"  → "30 day old files"
 * Falls back to extract_number() when neither pattern matches.          */
static int extract_days_smart(const char *query, char *out, size_t outsz) {
    static const char *patterns[] = {
        "than[[:space:]]+([0-9]+)",    /* older than 30 */
        "([0-9]+)[[:space:]]+days?",   /* 30 days       */
        NULL
    };

    for (int pi = 0; patterns[pi]; pi++) {
        regex_t  re;
        regmatch_t m[2];
        if (regcomp(&re, patterns[pi], REG_EXTENDED | REG_ICASE) != 0) continue;

        int matched = (regexec(&re, query, 2, m, 0) == 0);
        regfree(&re);

        if (matched && m[1].rm_so >= 0) {
            size_t len = (size_t)(m[1].rm_eo - m[1].rm_so);
            if (len >= outsz) len = outsz - 1;
            memcpy(out, query + m[1].rm_so, len);
            out[len] = '\0';
            return 1;
        }
    }

    return extract_number(query, out, outsz);   /* plain fallback */
}

/* Extract the last meaningful token (non-numeric, non-keyword) as a name. */
static int extract_name(const char *query, char *out, size_t outsz) {
    static const char *skip[] = {
        "find","delete","remove","clean","purge","show","list","get",
        "older","than","days","files","file","in","at","the","a","an",
        "for","with","from","to","of","and","or", NULL
    };
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", query);
    char *last = NULL;
    char *tok = strtok(buf, " \t\n");
    while (tok) {
        if (!isdigit((unsigned char)tok[0])) {
            int is_skip = 0;
            for (int i = 0; skip[i]; i++) {
                char low[64];
                str_lower(low, tok, sizeof(low));
                if (strcmp(low, skip[i]) == 0) { is_skip = 1; break; }
            }
            if (!is_skip) last = tok;
        }
        tok = strtok(NULL, " \t\n");
    }
    if (!last) return 0;
    snprintf(out, outsz, "%s", last);
    return 1;
}

/* Substitute one {placeholder} in src → dst. Returns 1 if replaced, 0 if not found. */
static int substitute(const char *src, const char *placeholder,
                      const char *value, char *dst, size_t dstsz) {
    char marker[64];
    snprintf(marker, sizeof(marker), "{%s}", placeholder);
    const char *pos = strstr(src, marker);
    if (!pos) return 0;

    size_t pre = (size_t)(pos - src);
    size_t mlen = strlen(marker);
    size_t vlen = strlen(value);

    if (pre + vlen + strlen(pos + mlen) + 1 > dstsz) return 0;

    memcpy(dst, src, pre);
    memcpy(dst + pre, value, vlen);
    strcpy(dst + pre + vlen, pos + mlen);
    return 1;
}

/* ── extract_args_for_entry ───────────────────────────────────────────────── */
/*
 * Extracts argument values from a natural-language query for a registry entry.
 *
 * Dispatch rules per placeholder name:
 *   "path"              → token starting with / ~ .
 *   "days" / "count"    → regex contextual ("than N" / "N days") then fallback
 *   "host" / "name"
 *   "file" / "pattern"  → last non-keyword, non-numeric token
 *
 * extracted[i] is set to the extracted value for entry->args[i], or "" if
 * nothing was found.  Returns the number of args for which a value was found.
 */
int extract_args_for_entry(RegistryEntry *entry, const char *query,
                            char extracted[][256], int max_args) {
    int found_count = 0;
    int limit = entry->arg_count < max_args ? entry->arg_count : max_args;

    for (int i = 0; i < limit; i++) {
        extracted[i][0] = '\0';
        const char *argname = entry->args[i];
        int found = 0;

        if (strcmp(argname, "path") == 0) {
            found = extract_path(query, extracted[i], 256);
        } else if (strcmp(argname, "days") == 0 ||
                   strcmp(argname, "count") == 0) {
            found = extract_days_smart(query, extracted[i], 256);
        } else if (strcmp(argname, "host")    == 0 ||
                   strcmp(argname, "name")    == 0 ||
                   strcmp(argname, "file")    == 0 ||
                   strcmp(argname, "pattern") == 0) {
            found = extract_name(query, extracted[i], 256);
        }

        if (found) found_count++;
    }
    return found_count;
}

/* ── registry_build_command ───────────────────────────────────────────────── */

char *registry_build_command(RegistryEntry *entry, const char *query) {
    char result[1024];
    snprintf(result, sizeof(result), "%s", entry->command);

    if (entry->arg_count == 0) return strdup(result);

    /* Extract all argument values in one pass */
    char extracted[REGISTRY_MAX_ARGS][256];
    memset(extracted, 0, sizeof(extracted));
    extract_args_for_entry(entry, query, extracted, REGISTRY_MAX_ARGS);

    /* Substitute each {placeholder} — report the first missing one */
    for (int i = 0; i < entry->arg_count; i++) {
        if (extracted[i][0] == '\0') {
            fprintf(stdout,
                "[registry] Missing argument <%s> — example:\n"
                "[registry]   @ %s\n"
                "[registry]   (provide a %s in your query)\n",
                entry->args[i], entry->description, entry->args[i]);
            return NULL;
        }
        char tmp[1024];
        if (substitute(result, entry->args[i], extracted[i], tmp, sizeof(tmp)))
            snprintf(result, sizeof(result), "%s", tmp);
    }

    return strdup(result);
}

/* ── registry_list ────────────────────────────────────────────────────────── */

void registry_list(void) {
    if (g_registry_count == 0) {
        printf("[registry] No commands loaded. Check commands.json.\n");
        return;
    }
    printf("[registry] %d commands loaded:\n\n", g_registry_count);
    printf("  %-30s %-15s %s\n", "ID", "CATEGORY", "DESCRIPTION");
    printf("  %-30s %-15s %s\n",
           "------------------------------",
           "---------------",
           "-----------------------------");
    for (int i = 0; i < g_registry_count; i++) {
        RegistryEntry *e = &g_registry[i];
        printf("  %-30s %-15s %s\n",
               e->id, e->category, e->description);
        /* Show first two aliases as examples */
        for (int j = 0; j < e->alias_count && j < 2; j++)
            printf("    alias: \"%s\"\n", e->aliases[j]);
    }
}
