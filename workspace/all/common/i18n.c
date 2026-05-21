#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#include "defines.h"
#include "utils.h"
#include "i18n.h"

#define I18N_BUCKETS    1024
#define I18N_ARENA_SIZE (32 * 1024)
#define I18N_LINE_MAX   1024

typedef struct {
    uint32_t    hash;
    const char *key;
    const char *value;
} i18n_entry;

static i18n_entry s_table[I18N_BUCKETS];
static char       s_arena[I18N_ARENA_SIZE];
static size_t     s_arena_used;
static char       s_active_code[I18N_LANG_CODE_MAX];
static int        s_inited;

static uint32_t fnv1a(const char *s) {
    uint32_t h = 0x811c9dc5u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 0x01000193u;
    }
    return h;
}

static const char *arena_dup(const char *s, size_t len) {
    if (s_arena_used + len + 1 > I18N_ARENA_SIZE) return NULL;
    char *p = s_arena + s_arena_used;
    memcpy(p, s, len);
    p[len] = '\0';
    s_arena_used += len + 1;
    return p;
}

static void table_clear(void) {
    memset(s_table, 0, sizeof(s_table));
    s_arena_used = 0;
}

static int table_insert(const char *key, size_t klen, const char *val, size_t vlen) {
    char keybuf[256];
    if (klen >= sizeof(keybuf)) return 0;
    memcpy(keybuf, key, klen);
    keybuf[klen] = '\0';

    uint32_t h = fnv1a(keybuf);
    if (h == 0) h = 1;

    size_t mask = I18N_BUCKETS - 1;
    size_t i    = h & mask;
    for (size_t probe = 0; probe < I18N_BUCKETS; ++probe) {
        i18n_entry *e = &s_table[i];
        if (e->hash == 0) {
            const char *dk = arena_dup(key, klen);
            const char *dv = arena_dup(val, vlen);
            if (!dk || !dv) return 0;
            e->hash  = h;
            e->key   = dk;
            e->value = dv;
            return 1;
        }
        if (e->hash == h && strcmp(e->key, keybuf) == 0) {
            const char *dv = arena_dup(val, vlen);
            if (!dv) return 0;
            e->value = dv;
            return 1;
        }
        i = (i + 1) & mask;
    }
    return 0;
}

static void strip_trailing_ws(char *s, size_t *len) {
    while (*len > 0 && (s[*len - 1] == ' ' || s[*len - 1] == '\t' ||
                        s[*len - 1] == '\r' || s[*len - 1] == '\n')) {
        s[--(*len)] = '\0';
    }
}

static int parse_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[I18N_LINE_MAX];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '\0' || *p == '#' || *p == '\n' || *p == '\r') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;

        size_t klen = (size_t)(eq - p);
        while (klen > 0 && (p[klen - 1] == ' ' || p[klen - 1] == '\t')) --klen;
        if (klen == 0) continue;

        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') ++v;

        size_t vlen = strlen(v);
        strip_trailing_ws(v, &vlen);

        table_insert(p, klen, v, vlen);
    }
    fclose(f);
    return 1;
}

static int load_lang(const char *lang_code) {
    table_clear();

    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/en.lang", SDCARD_PATH I18N_LANG_DIR);
    parse_file(path);

    if (lang_code && *lang_code && strcmp(lang_code, "en") != 0) {
        snprintf(path, sizeof(path), "%s/%s.lang",
                 SDCARD_PATH I18N_LANG_DIR, lang_code);
        parse_file(path);
    }

    strncpy(s_active_code, (lang_code && *lang_code) ? lang_code : "en",
            sizeof(s_active_code) - 1);
    s_active_code[sizeof(s_active_code) - 1] = '\0';
    return 1;
}

void I18N_init(const char *lang_code) {
    if (s_inited) return;
    s_inited = 1;
    load_lang(lang_code);
}

int I18N_reload(const char *lang_code) {
    return load_lang(lang_code);
}

void I18N_quit(void) {
    table_clear();
    s_active_code[0] = '\0';
    s_inited         = 0;
}

char *I18N_t(const char *key) {
    static char empty[1] = { '\0' };
    if (!key) return empty;
    if (!s_inited) return (char *)key;

    uint32_t h = fnv1a(key);
    if (h == 0) h = 1;

    size_t mask = I18N_BUCKETS - 1;
    size_t i    = h & mask;
    for (size_t probe = 0; probe < I18N_BUCKETS; ++probe) {
        i18n_entry *e = &s_table[i];
        if (e->hash == 0) return (char *)key;
        if (e->hash == h && strcmp(e->key, key) == 0) return (char *)e->value;
        i = (i + 1) & mask;
    }
    return (char *)key;
}

const char *I18N_active_code(void) {
    return s_active_code[0] ? s_active_code : "en";
}
