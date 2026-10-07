/*
 * SimpletonOS UI - the settings files (implementation). See config.h.
 */
#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

const char * config_dir(void)
{
    static char dir[256];
    if(!dir[0]) {
        const char * env = getenv("SIMPLETON_CONFIG");
        snprintf(dir, sizeof(dir), "%s", env && env[0] ? env : CONFIG_DIR_DEFAULT);
    }
    return dir;
}

void config_path(const char * name, char * out, size_t len)
{
    snprintf(out, len, "%s/%s", config_dir(), name);
}

char * config_read(const char * name, char * out, size_t len, const char * fallback)
{
    if(len == 0) return out;
    char path[512];
    config_path(name, path, sizeof(path));
    FILE * f = fopen(path, "r");
    out[0] = 0;
    if(f) {
        if(!fgets(out, (int)len, f)) out[0] = 0;
        fclose(f);
    }
    /* trim both ends: editors leave newlines, people leave spaces */
    char * s = out;
    while(*s && isspace((unsigned char)*s)) s++;
    if(s != out) memmove(out, s, strlen(s) + 1);
    size_t n = strlen(out);
    while(n && isspace((unsigned char)out[n - 1])) out[--n] = 0;
    if(!out[0] && fallback) snprintf(out, len, "%s", fallback);
    return out;
}

bool config_read_bool(const char * name, bool fallback)
{
    char v[32];
    config_read(name, v, sizeof(v), NULL);
    if(!v[0]) return fallback;
    return strcmp(v, "1") == 0 || strcasecmp(v, "on") == 0 || strcasecmp(v, "yes") == 0 || strcasecmp(v, "true") == 0;
}

bool config_write(const char * name, const char * value)
{
    const char * dir = config_dir();
    if(mkdir(dir, 0755) != 0 && errno != EEXIST) {
        /* the parent may be missing too (first boot, tests): one level up */
        char parent[256];
        snprintf(parent, sizeof(parent), "%s", dir);
        char * slash = strrchr(parent, '/');
        if(slash && slash != parent) { *slash = 0; mkdir(parent, 0755); }
        if(mkdir(dir, 0755) != 0 && errno != EEXIST) return false;
    }
    char path[512], tmp[520];
    config_path(name, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE * f = fopen(tmp, "w");
    if(!f) return false;
    bool ok = fprintf(f, "%s\n", value) > 0 && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = (fclose(f) == 0) && ok;
    if(ok) ok = rename(tmp, path) == 0;
    if(!ok) unlink(tmp);
    return ok;
}

const char * config_share_dir(void)
{
    static char dir[256];
    if(!dir[0]) {
        const char * env = getenv("SIMPLETON_SHARE");
        snprintf(dir, sizeof(dir), "%s", env && env[0] ? env : SHARE_DIR_DEFAULT);
    }
    return dir;
}

const char * config_theme_name(void)
{
    static char name[32];
    if(!name[0]) {
        config_read("theme", name, sizeof(name), THEME_DEFAULT);
        /* a name is a word: no path tricks from a hand-edited file */
        for(char * c = name; *c; c++)
            if(!isalnum((unsigned char)*c) && *c != '-' && *c != '_') { snprintf(name, sizeof(name), "%s", THEME_DEFAULT); break; }
    }
    return name;
}

void config_theme_file(char * out, size_t len)
{
    snprintf(out, len, "%s/themes/%s.theme", config_share_dir(), config_theme_name());
}

void config_theme_asset(const char * file, char * out, size_t len)
{
    if(file[0] == '/') snprintf(out, len, "%s", file);
    else snprintf(out, len, "%s/themes/%s/%s", config_share_dir(), config_theme_name(), file);
}
