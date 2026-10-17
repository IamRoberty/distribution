/*
 * SimpletonOS UI - favourites, mixtapes and collections (implementation).
 * See curation.h.
 */
#define _GNU_SOURCE
#include "curation.h"
#include "libs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define MUSIC_DIR_DEFAULT  "/storage/music"
#define MIRROR_DEFAULT     "/storage/.simpleton/curation"
#define CARD_DIR           ".simpleton/curation"
#define FAV_FILE           "favorites.m3u8"
#define MIX_DIR            "mixtapes"
#define COL_DIR            "collections"
#define MIX_EXT            ".m3u8"
#define COL_EXT            ".txt"
#define MAX_FILE           (4 * 1024 * 1024)
#define MAX_IDS            1000

typedef struct {
    char    card[64];
    char ** fav;
    int     nfav, capfav;
} cur_card_t;

static cur_card_t   cards[LIBS_MAX];
static int          ncards;
static cur_list_t ** lists;           /* pointers: a list stays where it is until deleted */
static int          nlists, caplists;
static unsigned     generation = 1, libs_seen;
static bool         loaded_once;
static int          next_uid = 1;
static unsigned     max_seq;
static int *        order[2];
static int          norder[2];
static unsigned     order_gen[2];

/* ---------------------------------------------------------------- helpers */

static char * xstrdup(const char * s) { return strdup(s ? s : ""); }

typedef struct { char * s; size_t n, cap; bool bad; } sb_t;

static void sb_add(sb_t * b, const char * s)
{
    size_t l = strlen(s);
    if(b->bad) return;
    if(b->n + l + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 1024;
        while(c < b->n + l + 1) c *= 2;
        char * p = realloc(b->s, c);
        if(!p) { b->bad = true; return; }
        b->s = p; b->cap = c;
    }
    memcpy(b->s + b->n, s, l + 1);
    b->n += l;
}

static void sb_printf(sb_t * b, const char * fmt, ...)
{
    char buf[2400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sb_add(b, buf);
}

static bool push_str(char *** arr, int * n, int * cap, const char * s)
{
    if(*n >= *cap) {
        int c = *cap ? *cap * 2 : 16;
        char ** p = realloc(*arr, sizeof(char *) * (size_t)c);
        if(!p) return false;
        *arr = p; *cap = c;
    }
    (*arr)[(*n)++] = xstrdup(s);
    return true;
}

static const char * music_dir(void)
{
    const char * e = getenv("SIMPLETON_MUSIC_DIR");
    return e && e[0] ? e : MUSIC_DIR_DEFAULT;
}

/* Where a card is mounted: MPD's music folder holds a link card-<serial>
 * to the mount (as cache.c finds it). */
static void card_mount(const char * card, char * out, size_t len)
{
    char link[1200];
    snprintf(link, sizeof(link), "%s/%s", music_dir(), card);
    char target[1200];
    ssize_t n = readlink(link, target, sizeof(target) - 1);
    if(n <= 0) { snprintf(out, len, "%s", link); return; }
    target[n] = 0;
    if(target[0] == '/') snprintf(out, len, "%s", target);
    else snprintf(out, len, "%s/%.1000s", music_dir(), target);
}

void curation_dir(const char * card, char * out, size_t len)
{
    char mount[1200];
    card_mount(card, mount, sizeof(mount));
    snprintf(out, len, "%s/%s", mount, CARD_DIR);
}

void curation_mirror_dir(const char * card, char * out, size_t len)
{
    const char * e = getenv("SIMPLETON_CURATION_MIRROR");
    snprintf(out, len, "%s/%s", e && e[0] ? e : MIRROR_DEFAULT, card);
}

static bool is_dir(const char * p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

static bool mkdirs(const char * path)
{
    char p[1600];
    snprintf(p, sizeof(p), "%s", path);
    for(char * s = p + 1; *s; s++) {
        if(*s != '/') continue;
        *s = 0;
        if(mkdir(p, 0755) != 0 && errno != EEXIST) { *s = '/'; return false; }
        *s = '/';
    }
    return mkdir(p, 0755) == 0 || errno == EEXIST;
}

static void sync_dir_of(const char * path)
{
    char d[1600];
    snprintf(d, sizeof(d), "%s", path);
    char * slash = strrchr(d, '/');
    if(!slash) return;
    *slash = 0;
    int fd = open(d, O_RDONLY | O_DIRECTORY);
    if(fd >= 0) { fsync(fd); close(fd); }
}

/* Write a whole file safely: <path>.tmp, flushed to the medium, the old
 * file kept as <path>.bak (keep_bak), the new one renamed in. */
static bool write_safely(const char * path, const char * text, bool keep_bak)
{
    char tmp[1600], bak[1600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    snprintf(bak, sizeof(bak), "%s.bak", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(fd < 0) return false;
    size_t len = strlen(text), off = 0;
    while(off < len) {
        ssize_t w = write(fd, text + off, len - off);
        if(w <= 0) { close(fd); unlink(tmp); return false; }
        off += (size_t)w;
    }
    if(fsync(fd) != 0) { close(fd); unlink(tmp); return false; }
    if(close(fd) != 0) { unlink(tmp); return false; }
    bool backed = false;
    if(keep_bak) backed = rename(path, bak) == 0;
    if(rename(tmp, path) != 0) {
        int e = errno;
        unlink(tmp);
        if(backed) rename(bak, path);
        errno = e;
        return false;
    }
    sync_dir_of(path);
    return true;
}

static char * read_file(const char * path)
{
    FILE * f = fopen(path, "rb");
    if(!f) return NULL;
    sb_t b = {0};
    char buf[8192];
    size_t n;
    while((n = fread(buf, 1, sizeof(buf) - 1, f)) > 0) {
        buf[n] = 0;
        if(memchr(buf, 0, n)) { fclose(f); free(b.s); return NULL; }   /* not text */
        sb_add(&b, buf);
        if(b.n > MAX_FILE) { fclose(f); free(b.s); return NULL; }
    }
    bool err = ferror(f);
    fclose(f);
    if(err || b.bad) { free(b.s); return NULL; }
    if(!b.s) return xstrdup("");
    return b.s;
}

/* The file, else what a save cut short left: the flushed .tmp, else the
 * .bak. `which` says where it came from (0 the file). */
static char * read_best(const char * path, int * which)
{
    char alt[1600];
    char * t = read_file(path);
    if(t) { if(which) *which = 0; return t; }
    snprintf(alt, sizeof(alt), "%s.tmp", path);
    if((t = read_file(alt))) { if(which) *which = 1; return t; }
    snprintf(alt, sizeof(alt), "%s.bak", path);
    if((t = read_file(alt))) { if(which) *which = 2; return t; }
    return NULL;
}

static void unlink_all(const char * path)
{
    char p[1600];
    unlink(path);
    snprintf(p, sizeof(p), "%s.tmp", path); unlink(p);
    snprintf(p, sizeof(p), "%s.bak", path); unlink(p);
}

const char * curation_card_of(const char * uri, char * out, size_t len)
{
    out[0] = 0;
    if(!uri || strncmp(uri, "card-", 5) != 0) return out;
    const char * slash = strchr(uri, '/');
    size_t l = slash ? (size_t)(slash - uri) : strlen(uri);
    if(l >= len) l = len - 1;
    memcpy(out, uri, l);
    out[l] = 0;
    return out;
}

static int card_index(const char * card)
{
    for(int i = 0; i < ncards; i++) if(strcmp(cards[i].card, card) == 0) return i;
    return -1;
}

/* A playlist line as a URI on `card` (or on `foreign` when a
 * #SIMPLETON-CARD line came before it): backslashes become slashes, a
 * drive letter, a leading slash and any "./" or "../" go. */
static bool line_uri(const char * card, const char * foreign, const char * line, char * out, size_t len)
{
    char p[2048];
    snprintf(p, sizeof(p), "%s", line);
    for(char * s = p; *s; s++) if(*s == '\\') *s = '/';
    const char * s = p;
    if(((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) && s[1] == ':') s += 2;
    for(;;) {
        if(s[0] == '/') { s++; continue; }
        if(s[0] == '.' && s[1] == '/') { s += 2; continue; }
        if(s[0] == '.' && s[1] == '.' && s[2] == '/') { s += 3; continue; }
        break;
    }
    if(!s[0]) return false;
    if(strncmp(s, "file://", 7) == 0 || strstr(s, "://")) return false;   /* a stream: not ours */
    snprintf(out, len, "%s/%s", foreign && foreign[0] ? foreign : card, s);
    return true;
}

static char * next_line(char ** cursor)
{
    char * s = *cursor;
    if(!s || !*s) return NULL;
    char * e = strchr(s, '\n');
    if(e) { *e = 0; *cursor = e + 1; } else *cursor = s + strlen(s);
    size_t l = strlen(s);
    while(l > 0 && (s[l - 1] == '\r' || s[l - 1] == ' ' || s[l - 1] == '\t')) s[--l] = 0;
    if((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s += 3;
    return s;
}

/* ---------------------------------------------------------------- lists */

static void list_free(cur_list_t * l)
{
    if(!l) return;
    for(int i = 0; i < l->n; i++) { free(l->uris[i]); if(l->titles) free(l->titles[i]); if(l->artists) free(l->artists[i]); }
    free(l->uris); free(l->titles); free(l->artists); free(l->name);
    free(l);
}

static bool list_push(cur_list_t * l, const char * uri, const char * title, const char * artist)
{
    if(l->n >= l->cap) {
        int c = l->cap ? l->cap * 2 : 16;
        char ** u = realloc(l->uris, sizeof(char *) * (size_t)c);
        if(!u) return false;
        l->uris = u;
        if(l->kind == CUR_COLLECTION) {
            char ** t = realloc(l->titles, sizeof(char *) * (size_t)c);
            if(!t) return false;
            l->titles = t;
            char ** a = realloc(l->artists, sizeof(char *) * (size_t)c);
            if(!a) return false;
            l->artists = a;
        }
        l->cap = c;
    }
    l->uris[l->n] = xstrdup(uri);
    if(l->kind == CUR_COLLECTION) { l->titles[l->n] = xstrdup(title); l->artists[l->n] = xstrdup(artist); }
    l->n++;
    return true;
}

static cur_list_t * list_add(cur_kind_t k, const char * card, const char * id)
{
    if(nlists >= caplists) {
        int c = caplists ? caplists * 2 : 32;
        cur_list_t ** p = realloc(lists, sizeof(cur_list_t *) * (size_t)c);
        if(!p) return NULL;
        lists = p; caplists = c;
    }
    cur_list_t * l = calloc(1, sizeof(*l));
    if(!l) return NULL;
    l->kind = k;
    snprintf(l->card, sizeof(l->card), "%s", card);
    snprintf(l->id, sizeof(l->id), "%s", id);
    lists[nlists++] = l;
    return l;
}

static void sub_path(const cur_list_t * l, const char * dir, char * out, size_t len)
{
    snprintf(out, len, "%s/%s/%s%s", dir, l->kind == CUR_MIXTAPE ? MIX_DIR : COL_DIR, l->id, l->kind == CUR_MIXTAPE ? MIX_EXT : COL_EXT);
}

/* ---------------------------------------------------------------- text */

/* A track line: relative to the playlist file (`up` "../" steps to the
 * card's root); a track on another card carries that card's name. */
static void m3u_line(sb_t * b, const char * card, const char * uri, int up)
{
    char c[64];
    curation_card_of(uri, c, sizeof(c));
    const char * rest = strchr(uri, '/');
    rest = rest ? rest + 1 : uri;
    if(strcmp(c, card) != 0) { sb_printf(b, "#SIMPLETON-CARD:%s\n%s\n", c, rest); return; }
    for(int i = 0; i < up; i++) sb_add(b, "../");
    sb_add(b, rest);
    sb_add(b, "\n");
}

static char * favorites_text(const cur_card_t * c)
{
    sb_t b = {0};
    sb_add(&b, "#EXTM3U\n#PLAYLIST:Favorites\n");
    for(int i = 0; i < c->nfav; i++) m3u_line(&b, c->card, c->fav[i], 2);
    if(b.bad) { free(b.s); return NULL; }
    return b.s;
}

static char * list_text(const cur_list_t * l)
{
    sb_t b = {0};
    if(l->kind == CUR_MIXTAPE) {
        sb_printf(&b, "#EXTM3U\n#PLAYLIST:%s\n#SIMPLETON-SEQ:%u\n", l->name ? l->name : l->id, l->seq);
        for(int i = 0; i < l->n; i++) m3u_line(&b, l->card, l->uris[i], 3);
    }
    else {
        sb_printf(&b, "#SIMPLETON-COLLECTION\n#NAME:%s\n#SEQ:%u\n", l->name ? l->name : l->id, l->seq);
        for(int i = 0; i < l->n; i++) {
            char c[64];
            curation_card_of(l->uris[i], c, sizeof(c));
            const char * rest = strchr(l->uris[i], '/');
            rest = rest ? rest + 1 : "";
            if(strcmp(c, l->card) != 0) sb_printf(&b, "#SIMPLETON-CARD:%s\n", c);
            sb_printf(&b, "%s\t%s\t%s\n", rest, l->artists[i], l->titles[i]);
        }
    }
    if(b.bad) { free(b.s); return NULL; }
    return b.s;
}

static void parse_favorites(cur_card_t * c, char * text)
{
    char * cur = text, * line, foreign[64] = "";
    while((line = next_line(&cur))) {
        if(!line[0]) continue;
        if(strncmp(line, "#SIMPLETON-CARD:", 16) == 0) { snprintf(foreign, sizeof(foreign), "%s", line + 16); continue; }
        if(line[0] == '#') continue;
        char uri[2200];
        if(line_uri(c->card, foreign, line, uri, sizeof(uri))) {
            bool dup = false;
            for(int i = 0; i < c->nfav && !dup; i++) dup = strcmp(c->fav[i], uri) == 0;
            if(!dup) push_str(&c->fav, &c->nfav, &c->capfav, uri);
        }
        foreign[0] = 0;
    }
}

static void parse_list(cur_list_t * l, char * text)
{
    char * cur = text, * line, foreign[64] = "";
    while((line = next_line(&cur))) {
        if(!line[0]) continue;
        if(strncmp(line, "#SIMPLETON-CARD:", 16) == 0) { snprintf(foreign, sizeof(foreign), "%s", line + 16); continue; }
        if(strncmp(line, "#SIMPLETON-SEQ:", 15) == 0 || strncmp(line, "#SEQ:", 5) == 0) {
            l->seq = (unsigned)strtoul(strchr(line, ':') + 1, NULL, 10);
            continue;
        }
        if(strncmp(line, "#PLAYLIST:", 10) == 0 || strncmp(line, "#NAME:", 6) == 0) {
            const char * v = strchr(line, ':') + 1;
            while(*v == ' ') v++;
            if(*v) { free(l->name); l->name = xstrdup(v); }
            continue;
        }
        if(line[0] == '#') continue;
        if(l->kind == CUR_MIXTAPE) {
            char uri[2200];
            if(line_uri(l->card, foreign, line, uri, sizeof(uri))) list_push(l, uri, NULL, NULL);
        }
        else {
            char * t1 = strchr(line, '\t');
            char * artist = (char *)"", * title = (char *)"";
            if(t1) {
                *t1 = 0; artist = t1 + 1;
                char * t2 = strchr(artist, '\t');
                if(t2) { *t2 = 0; title = t2 + 1; }
            }
            size_t fl = strlen(line);
            while(fl > 0 && line[fl - 1] == '/') line[--fl] = 0;
            char uri[2200];
            if(line_uri(l->card, foreign, line, uri, sizeof(uri))) list_push(l, uri, title, artist);
        }
        foreign[0] = 0;
    }
    if(!l->name) l->name = xstrdup(l->id);
    if(l->seq > max_seq) max_seq = l->seq;
}

/* ---------------------------------------------------------------- saving */

static bool save_text(const char * card, const char * rel, const char * text)
{
    char dir[1400], mir[1400], path[1600];
    curation_dir(card, dir, sizeof(dir));
    curation_mirror_dir(card, mir, sizeof(mir));
    /* the unit's copy first: it is the one that can't fail for want of a card */
    snprintf(path, sizeof(path), "%s/%s", mir, rel);
    { char d[1600]; snprintf(d, sizeof(d), "%s", path); char * s = strrchr(d, '/'); if(s) { *s = 0; mkdirs(d); } }
    if(!write_safely(path, text, false)) fprintf(stderr, "simpleton-ui: curation: couldn't keep the unit's copy %s: %s\n", path, strerror(errno));
    snprintf(path, sizeof(path), "%s/%s", dir, rel);
    { char d[1600]; snprintf(d, sizeof(d), "%s", path); char * s = strrchr(d, '/'); if(s) { *s = 0; if(!mkdirs(d)) { fprintf(stderr, "simpleton-ui: curation: can't make %s: %s\n", d, strerror(errno)); return false; } } }
    if(!write_safely(path, text, true)) { fprintf(stderr, "simpleton-ui: curation: couldn't save %s: %s\n", path, strerror(errno)); return false; }
    return true;
}

static bool save_favorites(int ci)
{
    char * t = favorites_text(&cards[ci]);
    if(!t) return false;
    bool ok = save_text(cards[ci].card, FAV_FILE, t);
    free(t);
    return ok;
}

static bool save_list(const cur_list_t * l)
{
    char * t = list_text(l);
    if(!t) return false;
    char rel[200];
    snprintf(rel, sizeof(rel), "%s/%s%s", l->kind == CUR_MIXTAPE ? MIX_DIR : COL_DIR, l->id, l->kind == CUR_MIXTAPE ? MIX_EXT : COL_EXT);
    bool ok = save_text(l->card, rel, t);
    free(t);
    return ok;
}

/* ---------------------------------------------------------------- loading */

/* The ids in a folder: names ending in ext, ext.tmp or ext.bak, once each. */
static int scan_ids(const char * dir, const char * ext, char (*ids)[64], int max)
{
    DIR * d = opendir(dir);
    if(!d) return 0;
    int n = 0;
    size_t el = strlen(ext);
    struct dirent * de;
    while((de = readdir(d)) && n < max) {
        const char * nm = de->d_name;
        if(nm[0] == '.') continue;
        size_t l = strlen(nm);
        size_t stem = 0;
        if(l > el && strcasecmp(nm + l - el, ext) == 0) stem = l - el;
        else if(l > el + 4 && (strcmp(nm + l - 4, ".tmp") == 0 || strcmp(nm + l - 4, ".bak") == 0) && strncasecmp(nm + l - 4 - el, ext, el) == 0) stem = l - 4 - el;
        if(stem == 0 || stem >= 64) continue;
        char id[64];
        memcpy(id, nm, stem);
        id[stem] = 0;
        bool dup = false;
        for(int i = 0; i < n && !dup; i++) dup = strcmp(ids[i], id) == 0;
        if(!dup) snprintf(ids[n++], 64, "%s", id);
    }
    closedir(d);
    return n;
}

/* Copy every file of the unit's copy onto the card (its curation folder
 * was missing altogether: a card wiped, or a new card in its place). */
static void restore_card(const char * card, const char * from)
{
    int restored = 0;
    char path[1600];
    snprintf(path, sizeof(path), "%s/%s", from, FAV_FILE);
    char * t = read_file(path);
    if(t) { if(save_text(card, FAV_FILE, t)) restored++; free(t); }
    for(int k = 0; k < 2; k++) {
        const char * sub = k == 0 ? MIX_DIR : COL_DIR, * ext = k == 0 ? MIX_EXT : COL_EXT;
        char dir[1500];
        snprintf(dir, sizeof(dir), "%s/%s", from, sub);
        static char ids[MAX_IDS][64];
        int n = scan_ids(dir, ext, ids, MAX_IDS);
        for(int i = 0; i < n; i++) {
            snprintf(path, sizeof(path), "%s/%s%s", dir, ids[i], ext);
            if(!(t = read_file(path))) continue;
            char rel[200];
            snprintf(rel, sizeof(rel), "%s/%s%s", sub, ids[i], ext);
            if(save_text(card, rel, t)) restored++;
            free(t);
        }
    }
    fprintf(stderr, "simpleton-ui: curation: %s had no curation folder: %d file%s put back from this unit's copy\n", card, restored, restored == 1 ? "" : "s");
}

/* The unit's copy of a file read off the card, kept up to date (a
 * playlist dropped in from a PC, a card curated on another unit). */
static void mirror_text(const char * card, const char * rel, const char * text)
{
    char mir[1400], path[1600];
    curation_mirror_dir(card, mir, sizeof(mir));
    snprintf(path, sizeof(path), "%s/%s", mir, rel);
    char * have = read_file(path);
    bool same = have && strcmp(have, text) == 0;
    free(have);
    if(same) return;
    char d[1600]; snprintf(d, sizeof(d), "%s", path); char * sl = strrchr(d, '/'); if(sl) { *sl = 0; mkdirs(d); }
    write_safely(path, text, false);
}

static void load_card(const char * card, int * nfav, int * nmix, int * ncol)
{
    if(ncards >= LIBS_MAX) return;
    cur_card_t * c = &cards[ncards++];
    memset(c, 0, sizeof(*c));
    snprintf(c->card, sizeof(c->card), "%s", card);
    char dir[1400], mir[1400], path[1600];
    curation_dir(card, dir, sizeof(dir));
    curation_mirror_dir(card, mir, sizeof(mir));
    if(!is_dir(dir) && is_dir(mir)) restore_card(card, mir);
    const char * from = is_dir(dir) ? dir : mir;     /* a card that took nothing back: still show the unit's copy */

    int which = 0;
    snprintf(path, sizeof(path), "%s/%s", from, FAV_FILE);
    char * t = read_best(path, &which);
    if(t) {
        if(from == dir && !which) mirror_text(card, FAV_FILE, t);
        parse_favorites(c, t);
        free(t);
        if(which && from == dir) save_favorites((int)(c - cards));   /* a save was cut short: finish it */
    }
    *nfav += c->nfav;

    for(int k = 0; k < 2; k++) {
        cur_kind_t kind = k == 0 ? CUR_MIXTAPE : CUR_COLLECTION;
        const char * sub = k == 0 ? MIX_DIR : COL_DIR, * ext = k == 0 ? MIX_EXT : COL_EXT;
        char sdir[1500];
        snprintf(sdir, sizeof(sdir), "%s/%s", from, sub);
        static char ids[MAX_IDS][64];
        int n = scan_ids(sdir, ext, ids, MAX_IDS);
        for(int i = 0; i < n; i++) {
            snprintf(path, sizeof(path), "%s/%s%s", sdir, ids[i], ext);
            t = read_best(path, &which);
            if(!t) continue;
            cur_list_t * l = list_add(kind, card, ids[i]);
            if(!l) { free(t); continue; }
            if(from == dir && !which) { char rel[200]; snprintf(rel, sizeof(rel), "%s/%s%s", sub, ids[i], ext); mirror_text(card, rel, t); }
            parse_list(l, t);
            free(t);
            if(which && from == dir) save_list(l);
            if(k == 0) (*nmix)++; else (*ncol)++;
        }
    }
}

/* The cards mounted now (libs.h): read each one's store. A list that was
 * here before keeps its uid, so the views and the menu keep their place. */
static void ensure(void)
{
    if(loaded_once && libs_seen == libs_generation()) return;
    loaded_once = true;
    libs_seen = libs_generation();

    /* old uids by "<card>/<kind>/<id>" */
    int nold = nlists;
    cur_list_t ** old = lists;
    lists = NULL; nlists = caplists = 0;
    for(int i = 0; i < ncards; i++) {
        for(int k = 0; k < cards[i].nfav; k++) free(cards[i].fav[k]);
        free(cards[i].fav);
    }
    ncards = 0;
    max_seq = 0;

    int nfav = 0, nmix = 0, ncol = 0;
    for(int s = 0; s < libs_count(); s++) load_card(libs_get(s)->card, &nfav, &nmix, &ncol);
    for(int i = 0; i < nlists; i++) {
        lists[i]->uid = 0;
        for(int j = 0; j < nold; j++)
            if(old[j] && old[j]->kind == lists[i]->kind && strcmp(old[j]->card, lists[i]->card) == 0 && strcmp(old[j]->id, lists[i]->id) == 0) { lists[i]->uid = old[j]->uid; break; }
        if(!lists[i]->uid) lists[i]->uid = next_uid++;
    }
    for(int j = 0; j < nold; j++) list_free(old[j]);
    free(old);
    generation++;
    if(libs_count() > 0)
        fprintf(stderr, "simpleton-ui: curation: %d favourite%s, %d mixtape%s, %d collection%s on %d card%s\n",
                nfav, nfav == 1 ? "" : "s", nmix, nmix == 1 ? "" : "s", ncol, ncol == 1 ? "" : "s", ncards, ncards == 1 ? "" : "s");
}

unsigned curation_generation(void) { ensure(); return generation; }

/* ---------------------------------------------------------------- favourites */

bool curation_is_favorite(const char * uri)
{
    ensure();
    if(!uri) return false;
    for(int c = 0; c < ncards; c++)
        for(int i = 0; i < cards[c].nfav; i++) if(strcmp(cards[c].fav[i], uri) == 0) return true;
    return false;
}

int curation_favorite_count(void)
{
    ensure();
    int n = 0;
    for(int c = 0; c < ncards; c++) n += cards[c].nfav;
    return n;
}

const char * curation_favorite(int i)
{
    ensure();
    for(int c = 0; c < ncards; c++) {
        if(i < cards[c].nfav) return i >= 0 ? cards[c].fav[i] : NULL;
        i -= cards[c].nfav;
    }
    return NULL;
}

bool curation_set_favorite(const char * uri, bool on)
{
    ensure();
    char card[64];
    curation_card_of(uri, card, sizeof(card));
    int ci = card_index(card);
    if(ci < 0) return false;
    cur_card_t * c = &cards[ci];
    int at = -1;
    for(int i = 0; i < c->nfav && at < 0; i++) if(strcmp(c->fav[i], uri) == 0) at = i;
    if(on == (at >= 0)) return true;
    if(on) { if(!push_str(&c->fav, &c->nfav, &c->capfav, uri)) return false; }
    else { free(c->fav[at]); memmove(&c->fav[at], &c->fav[at + 1], sizeof(char *) * (size_t)(c->nfav - at - 1)); c->nfav--; }
    generation++;
    fprintf(stderr, "simpleton-ui: curation: %s %s\n", on ? "favourite" : "no longer a favourite:", uri);
    return save_favorites(ci);
}

/* ---------------------------------------------------------------- lists */

static int cmp_kind;
static int order_cmp(const void * pa, const void * pb)
{
    const cur_list_t * a = lists[*(const int *)pa], * b = lists[*(const int *)pb];
    if(a->seq != b->seq) return a->seq > b->seq ? -1 : 1;
    int c = strcasecmp(a->name ? a->name : "", b->name ? b->name : "");
    return c ? c : strcmp(a->id, b->id);
}

static void make_order(cur_kind_t k)
{
    if(order_gen[k] == generation && order[k]) return;
    free(order[k]);
    order[k] = malloc(sizeof(int) * (size_t)(nlists ? nlists : 1));
    norder[k] = 0;
    if(!order[k]) return;
    for(int i = 0; i < nlists; i++) if(lists[i]->kind == k) order[k][norder[k]++] = i;
    cmp_kind = (int)k;
    qsort(order[k], (size_t)norder[k], sizeof(int), order_cmp);
    order_gen[k] = generation;
}

int curation_count(cur_kind_t k) { ensure(); make_order(k); return norder[k]; }

const cur_list_t * curation_get(cur_kind_t k, int i)
{
    ensure();
    make_order(k);
    return (i >= 0 && i < norder[k]) ? lists[order[k][i]] : NULL;
}

static cur_list_t * by_uid(int uid)
{
    if(uid <= 0) return NULL;
    for(int i = 0; i < nlists; i++) if(lists[i]->uid == uid) return lists[i];
    return NULL;
}

const cur_list_t * curation_by_uid(int uid) { ensure(); return by_uid(uid); }

void curation_key(int uid, char * out, size_t len)
{
    ensure();
    const cur_list_t * l = by_uid(uid);
    if(l) snprintf(out, len, "%s/%s/%s", l->card, l->kind == CUR_MIXTAPE ? MIX_DIR : COL_DIR, l->id);
    else if(len) out[0] = 0;
}

int curation_find_key(const char * key)
{
    ensure();
    if(!key) return 0;
    for(int i = 0; i < nlists; i++) {
        char k[300];
        snprintf(k, sizeof(k), "%s/%s/%s", lists[i]->card, lists[i]->kind == CUR_MIXTAPE ? MIX_DIR : COL_DIR, lists[i]->id);
        if(strcmp(k, key) == 0) return lists[i]->uid;
    }
    return 0;
}

bool curation_name_used(cur_kind_t k, const char * name)
{
    ensure();
    for(int i = 0; i < nlists; i++) if(lists[i]->kind == k && lists[i]->name && strcasecmp(lists[i]->name, name) == 0) return true;
    return false;
}

int curation_new(cur_kind_t k, const char * card, const char * name)
{
    ensure();
    if(card_index(card) < 0) return 0;
    /* the first free id on that card, here and on the card itself */
    char dir[1400], sdir[1500], id[64] = "";
    curation_dir(card, dir, sizeof(dir));
    snprintf(sdir, sizeof(sdir), "%s/%s", dir, k == CUR_MIXTAPE ? MIX_DIR : COL_DIR);
    static char ids[MAX_IDS][64];
    int n = scan_ids(sdir, k == CUR_MIXTAPE ? MIX_EXT : COL_EXT, ids, MAX_IDS);
    for(int num = 1; num < 100000 && !id[0]; num++) {
        char cand[64];
        snprintf(cand, sizeof(cand), "%c%04d", k == CUR_MIXTAPE ? 'm' : 'c', num);
        bool used = false;
        for(int i = 0; i < n && !used; i++) used = strcasecmp(ids[i], cand) == 0;
        for(int i = 0; i < nlists && !used; i++) used = lists[i]->kind == k && strcmp(lists[i]->card, card) == 0 && strcasecmp(lists[i]->id, cand) == 0;
        if(!used) snprintf(id, sizeof(id), "%s", cand);
    }
    if(!id[0]) return 0;
    cur_list_t * l = list_add(k, card, id);
    if(!l) return 0;
    l->name = xstrdup(name);
    l->seq = ++max_seq;
    l->uid = next_uid++;
    generation++;
    bool ok = save_list(l);
    fprintf(stderr, "simpleton-ui: curation: new %s \"%s\" (%s/%s)%s\n", k == CUR_MIXTAPE ? "mixtape" : "collection", name, card, id, ok ? "" : ", not saved to the card");
    return l->uid;
}

bool curation_add_tracks(int uid, const char * const * uris, int n, int * added)
{
    ensure();
    if(added) *added = 0;
    cur_list_t * l = by_uid(uid);
    if(!l || l->kind != CUR_MIXTAPE) return false;
    int a = 0;
    for(int i = 0; i < n; i++) {
        if(!uris[i] || !uris[i][0]) continue;
        bool dup = false;
        for(int k = 0; k < l->n && !dup; k++) dup = strcmp(l->uris[k], uris[i]) == 0;
        if(dup) continue;
        if(list_push(l, uris[i], NULL, NULL)) a++;
    }
    if(added) *added = a;
    if(a == 0) return true;
    l->seq = ++max_seq;
    generation++;
    fprintf(stderr, "simpleton-ui: curation: %d track%s onto \"%s\"\n", a, a == 1 ? "" : "s", l->name);
    return save_list(l);
}

bool curation_contains_album(int uid, const char * folder, const char * title)
{
    ensure();
    const cur_list_t * l = by_uid(uid);
    if(!l || l->kind != CUR_COLLECTION) return false;
    for(int i = 0; i < l->n; i++) if(strcmp(l->uris[i], folder) == 0 && strcmp(l->titles[i], title ? title : "") == 0) return true;
    return false;
}

bool curation_add_album(int uid, const char * folder, const char * title, const char * artist, bool * added)
{
    ensure();
    if(added) *added = false;
    cur_list_t * l = by_uid(uid);
    if(!l || l->kind != CUR_COLLECTION || !folder || !folder[0]) return false;
    if(curation_contains_album(uid, folder, title)) return true;
    if(!list_push(l, folder, title, artist)) return false;
    if(added) *added = true;
    l->seq = ++max_seq;
    generation++;
    fprintf(stderr, "simpleton-ui: curation: \"%s\" into \"%s\"\n", title ? title : folder, l->name);
    return save_list(l);
}

bool curation_rename(int uid, const char * name)
{
    ensure();
    cur_list_t * l = by_uid(uid);
    if(!l || !name || !name[0]) return false;
    if(l->name && strcmp(l->name, name) == 0) return true;
    free(l->name);
    l->name = xstrdup(name);
    l->seq = ++max_seq;
    generation++;
    fprintf(stderr, "simpleton-ui: curation: %s/%s renamed \"%s\"\n", l->card, l->id, name);
    return save_list(l);
}

bool curation_remove_at(int uid, int pos)
{
    ensure();
    cur_list_t * l = by_uid(uid);
    if(!l || pos < 0 || pos >= l->n) return false;
    free(l->uris[pos]);
    if(l->titles) free(l->titles[pos]);
    if(l->artists) free(l->artists[pos]);
    int rest = l->n - pos - 1;
    memmove(&l->uris[pos], &l->uris[pos + 1], sizeof(char *) * (size_t)rest);
    if(l->titles) memmove(&l->titles[pos], &l->titles[pos + 1], sizeof(char *) * (size_t)rest);
    if(l->artists) memmove(&l->artists[pos], &l->artists[pos + 1], sizeof(char *) * (size_t)rest);
    l->n--;
    l->seq = ++max_seq;
    generation++;
    return save_list(l);
}

bool curation_delete(int uid)
{
    ensure();
    cur_list_t * l = by_uid(uid);
    if(!l) return false;
    char dir[1400], path[1600];
    curation_dir(l->card, dir, sizeof(dir));
    sub_path(l, dir, path, sizeof(path));
    unlink_all(path);
    bool gone = access(path, F_OK) != 0;
    curation_mirror_dir(l->card, dir, sizeof(dir));
    sub_path(l, dir, path, sizeof(path));
    unlink_all(path);
    fprintf(stderr, "simpleton-ui: curation: deleted \"%s\" (%s/%s)%s\n", l->name, l->card, l->id, gone ? "" : ", but the card's file is still there");
    for(int i = 0; i < nlists; i++) if(lists[i] == l) { memmove(&lists[i], &lists[i + 1], sizeof(cur_list_t *) * (size_t)(nlists - i - 1)); nlists--; break; }
    list_free(l);
    generation++;
    return gone;
}
