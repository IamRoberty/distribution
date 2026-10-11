/*
 * SimpletonOS UI - string table (implementation). See strings.h.
 */
#include "strings.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SHARE_DIR_DEFAULT "/usr/share/simpleton"
#define LANGUAGE_FILE     "/storage/.config/simpleton/language"
#define LANGUAGE_DEFAULT  "en"

static const char * const keys[S_COUNT] = {
#define STR(id, key, en) key,
#include "strings.def"
#undef STR
};

static const char * const english[S_COUNT] = {
#define STR(id, key, en) en,
#include "strings.def"
#undef STR
};

static char * loaded[S_COUNT];          /* from the language file; NULL = use English */
static char language[16] = LANGUAGE_DEFAULT;

const char * T(str_id_t id)
{
    if((unsigned)id >= S_COUNT) return "";
    return loaded[id] ? loaded[id] : english[id];
}

const char * strings_language(void) { return language; }

static char * trim(char * s)
{
    while(*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while(n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    return s;
}

/* Every {name} in `text` must also be in the English for that key: a
 * translation can drop a value but can't ask for one the code never gives. */
static bool places_known(const char * text, const char * en)
{
    for(const char * p = strchr(text, '{'); p; p = strchr(p + 1, '{')) {
        const char * q = strchr(p, '}');
        if(!q) return true;                      /* a lone brace is just text */
        size_t n = (size_t)(q - p) + 1;
        bool found = false;
        for(const char * e = strstr(en, "{"); e && !found; e = strstr(e + 1, "{"))
            if(strncmp(e, p, n) == 0) found = true;
        if(!found) return false;
    }
    return true;
}

/* Read one language file over whatever is loaded. Returns the number of
 * keys taken from it, or -1 when it can't be opened. */
static int load_file(const char * path)
{
    FILE * f = fopen(path, "r");
    if(!f) return -1;
    char line[1024];
    int taken = 0, lineno = 0;
    while(fgets(line, sizeof(line), f)) {
        lineno++;
        if(!strchr(line, '\n') && !feof(f)) {                   /* over-long line: keep its start, drop the rest */
            int c;
            while((c = fgetc(f)) != EOF && c != '\n') {}
        }
        char * s = line;
        if(lineno == 1 && strncmp(s, "\xEF\xBB\xBF", 3) == 0) s += 3;      /* Notepad's byte-order mark */
        s = trim(s);
        if(!s[0] || s[0] == '#') continue;
        char * eq = strchr(s, '=');
        if(!eq) { fprintf(stderr, "simpleton-ui: lang: %s:%d: no '=' (ignored)\n", path, lineno); continue; }
        *eq = 0;
        char * key = trim(s), * val = trim(eq + 1);
        int id = -1;
        for(int i = 0; i < S_COUNT; i++) if(strcmp(keys[i], key) == 0) { id = i; break; }
        if(id < 0) { fprintf(stderr, "simpleton-ui: lang: %s:%d: unknown key \"%s\" (ignored)\n", path, lineno, key); continue; }
        if(!val[0]) continue;                                    /* left blank: English */
        if(!places_known(val, english[id]))
            fprintf(stderr, "simpleton-ui: lang: %s:%d: \"%s\" uses a {place} the English doesn't have\n", path, lineno, key);
        char * copy = strdup(val);
        if(!copy) continue;
        free(loaded[id]);
        loaded[id] = copy;
        taken++;
    }
    fclose(f);
    return taken;
}

/* A language code is a file name: letters, digits, '-' and '_' only. */
static bool code_ok(const char * s)
{
    if(!s[0]) return false;
    for(; *s; s++) {
        char c = *s;
        if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    return true;
}

void strings_init(void)
{
    const char * share = getenv("SIMPLETON_SHARE");
    if(!share || !share[0]) share = SHARE_DIR_DEFAULT;

    char want[16] = "";
    const char * env = getenv("SIMPLETON_LANG");         /* testing: beats the setting */
    if(env && env[0]) snprintf(want, sizeof(want), "%s", env);
    else {
        FILE * f = fopen(LANGUAGE_FILE, "r");
        if(f) {
            if(fgets(want, sizeof(want), f)) want[strcspn(want, " \t\r\n")] = 0;
            fclose(f);
        }
    }
    if(!code_ok(want)) snprintf(want, sizeof(want), "%s", LANGUAGE_DEFAULT);

    char path[512];
    snprintf(path, sizeof(path), "%s/lang/%s.txt", share, want);
    int n = load_file(path);
    if(n < 0 && strcmp(want, LANGUAGE_DEFAULT) != 0) {
        fprintf(stderr, "simpleton-ui: lang: no %s, using %s\n", path, LANGUAGE_DEFAULT);
        snprintf(want, sizeof(want), "%s", LANGUAGE_DEFAULT);
        snprintf(path, sizeof(path), "%s/lang/%s.txt", share, want);
        n = load_file(path);
    }
    snprintf(language, sizeof(language), "%s", want);
    if(n < 0) fprintf(stderr, "simpleton-ui: lang: no %s, using built-in English (%d strings)\n", path, (int)S_COUNT);
    else      fprintf(stderr, "simpleton-ui: lang: %s, %d of %d strings from %s%s\n", language, n, (int)S_COUNT, path,
                      n < (int)S_COUNT ? " (the rest in English)" : "");
}

char * ui_strf(char * out, size_t len, str_id_t id, ...)
{
    if(!out || !len) return out;
    const char * names[8], * values[8];
    int argc = 0;
    va_list ap;
    va_start(ap, id);
    while(argc < 8) {
        const char * name = va_arg(ap, const char *);
        if(!name) break;
        const char * value = va_arg(ap, const char *);
        names[argc] = name;
        values[argc] = value ? value : "";
        argc++;
    }
    va_end(ap);

    const char * p = T(id);
    size_t o = 0;
    while(*p && o + 1 < len) {
        if(*p == '{') {
            const char * q = strchr(p, '}');
            int hit = -1;
            if(q) for(int i = 0; i < argc; i++)
                if(strlen(names[i]) == (size_t)(q - p - 1) && strncmp(names[i], p + 1, (size_t)(q - p - 1)) == 0) { hit = i; break; }
            if(hit >= 0) {
                for(const char * v = values[hit]; *v && o + 1 < len; v++) out[o++] = *v;
                p = q + 1;
                continue;
            }
        }
        out[o++] = *p++;
    }
    out[o] = 0;
    return out;
}

void strings_dump(FILE * f)
{
    fputs("# SimpletonOS interface text - English.\n"
          "#\n"
          "# One line per string:  key = text\n"
          "# To add a language, copy this file to <code>.txt (ru.txt, vi.txt ...) and\n"
          "# translate the text after each \"=\". Keep the keys as they are.\n"
          "# Words in {braces} are filled in by the player ({n} = a number, {dac} = the\n"
          "# DAC's name, {rate} = a sample rate): keep them, move them where they belong.\n"
          "# A line you leave out, or leave empty, shows in English.\n"
          "# Lines starting with # are notes. Save as UTF-8.\n", f);
    char group[32] = "";
    for(int i = 0; i < S_COUNT; i++) {
        size_t g = strcspn(keys[i], ".");
        if(g >= sizeof(group)) g = sizeof(group) - 1;
        if(strncmp(group, keys[i], g) != 0 || group[g] != 0) {
            snprintf(group, sizeof(group), "%.*s", (int)g, keys[i]);
            fputc('\n', f);
        }
        fprintf(f, "%s = %s\n", keys[i], english[i]);
    }
}

/* ------------------------------------------------------------ upper case */

/* The capital of a lower-case letter, 0 when it has none we change (11 Oct:
 * caps faces used to upper-case ASCII only, so "Björk" came out "BJöRK" and
 * a Russian title stayed lower case under a caps face). Only mappings that
 * keep the UTF-8 length are listed, so the text can change in place: Latin-1,
 * Latin Extended-A, Vietnamese and Romanian letters, Greek, Cyrillic,
 * Armenian, Latin Extended Additional, full-width Latin. ß, ı, ĸ, ſ stay. */
static uint32_t upper_of(uint32_t c)
{
    if(c >= 'a' && c <= 'z') return c - 32;
    if(c < 0xE0) return 0;
    if(c <= 0xFE) return c == 0xF7 ? 0 : c - 0x20;
    if(c == 0xFF) return 0x178;
    if(c <= 0x17F) {
        if(c == 0x131) return 0;                                         /* dotless i: its capital is ASCII I */
        if((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return (c & 1) ? c - 1 : 0;
        if((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? 0 : c - 1;
        return 0;
    }
    if(c == 0x1A1 || c == 0x1A3 || c == 0x1B0) return c - 1;            /* Vietnamese ơ ư */
    if((c >= 0x200 && c <= 0x21F) || (c >= 0x222 && c <= 0x233)) return (c & 1) ? c - 1 : 0;   /* ș ț */
    if(c >= 0x3B1 && c <= 0x3CB) return c == 0x3C2 ? 0x3A3 : c - 0x20; /* Greek, final sigma */
    if(c == 0x3AC) return 0x386;
    if(c >= 0x3AD && c <= 0x3AF) return c - 0x25;
    if(c == 0x3CC) return 0x38C;
    if(c >= 0x3CD && c <= 0x3CE) return c - 0x3F;
    if(c >= 0x430 && c <= 0x44F) return c - 0x20;                      /* Cyrillic */
    if(c >= 0x450 && c <= 0x45F) return c - 0x50;
    if((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F)) return (c & 1) ? c - 1 : 0;
    if(c >= 0x4C1 && c <= 0x4CE) return (c & 1) ? 0 : c - 1;
    if(c == 0x4CF) return 0x4C0;
    if(c >= 0x561 && c <= 0x586) return c - 0x30;                      /* Armenian */
    if((c >= 0x1E00 && c <= 0x1E95) || (c >= 0x1EA0 && c <= 0x1EFF)) return (c & 1) ? c - 1 : 0;
    if(c >= 0xFF41 && c <= 0xFF5A) return c - 0x20;                    /* full-width */
    return 0;
}

static int utf8_len(uint32_t c) { return c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4; }

void str_upper_utf8(char * s)
{
    unsigned char * p = (unsigned char *)s;
    while(*p) {
        int n = *p < 0x80 ? 1 : (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : (*p & 0xF8) == 0xF0 ? 4 : 1;
        uint32_t c = n == 1 ? *p : n == 2 ? (*p & 0x1Fu) : n == 3 ? (*p & 0x0Fu) : (*p & 0x07u);
        int k = 1;
        for(; k < n && (p[k] & 0xC0) == 0x80; k++) c = (c << 6) | (p[k] & 0x3Fu);
        if(k < n || (n == 1 && c >= 0x80)) { p += k; continue; }   /* broken sequence or stray byte: leave it */
        uint32_t u = upper_of(c);
        if(u && utf8_len(u) == n) {
            if(n == 1) p[0] = (unsigned char)u;
            else if(n == 2) { p[0] = (unsigned char)(0xC0 | (u >> 6)); p[1] = (unsigned char)(0x80 | (u & 0x3F)); }
            else if(n == 3) { p[0] = (unsigned char)(0xE0 | (u >> 12)); p[1] = (unsigned char)(0x80 | ((u >> 6) & 0x3F)); p[2] = (unsigned char)(0x80 | (u & 0x3F)); }
        }
        p += n;
    }
}
