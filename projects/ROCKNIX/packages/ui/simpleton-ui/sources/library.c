/*
 * SimpletonOS - the library index (implementation). See library.h.
 */
#define _GNU_SOURCE
#include "library.h"
#include "mpdc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const int lib_thumb_px[LIB_THUMB_COUNT] = { 192, 256, 384, 512 };

/* Folder-type thresholds (Note 05 section 7: "tuned later"). */
#define COLLECTION_MIN_TRACKS   20     /* a folder of singles                          */
#define COLLECTION_MIN_ARTISTS  5
#define ARTIST_MIN_ALBUMS       1      /* album folders inside (at least 2/3 of its music folders): a grid of covers */
#define LIBRARY_MIN_ARTISTS     2

/* ---------------------------------------------------------- small utils */

static char * xstrdup(const char * s) { char * d = strdup(s ? s : ""); if(!d) abort(); return d; }

static void * grow(void * p, int n, size_t sz)
{
    if(n & (n - 1)) return p;                     /* only at powers of two */
    void * q = realloc(p, (size_t)(n ? n * 2 : 1) * sz);
    if(!q) abort();
    return q;
}

static uint64_t fnv_str(uint64_t h, const char * s)
{
    for(; *s; s++) { h ^= (uint8_t)*s; h *= 1099511628211ull; }
    return h;
}

/* "card-x/Artist/Album/01.flac" -> "card-x/Artist/Album"; a container's
 * tracks ("Album/disc.iso/track03") belong to the folder holding it. */
static void folder_of(const char * uri, char * out, size_t len)
{
    const char * end = uri + strlen(uri);
    for(const char * p = uri; *p; p++) {
        if(*p == '.' && (strncasecmp(p, ".iso/", 5) == 0 || strncasecmp(p, ".cue/", 5) == 0 ||
                         strncasecmp(p, ".dff/", 5) == 0 || strncasecmp(p, ".dat/", 5) == 0)) { end = p; break; }
    }
    const char * slash = NULL;
    for(const char * p = uri; p < end; p++) if(*p == '/') slash = p;
    size_t n = slash ? (size_t)(slash - uri) : 0;
    snprintf(out, len, "%.*s", (int)n, uri);
}

static const char * base_name(const char * uri)
{
    const char * slash = strrchr(uri, '/');
    return slash ? slash + 1 : uri;
}

static int year_of(const char * original, const char * date)
{
    const char * s = original && original[0] ? original : date;
    if(!s || !s[0]) return 0;
    int y = atoi(s);
    return (y >= 1000 && y <= 9999) ? y : 0;
}

int library_name_cmp(const char * a, const char * b)
{
    if(strncasecmp(a, "the ", 4) == 0) a += 4;
    if(strncasecmp(b, "the ", 4) == 0) b += 4;
    return strcasecmp(a, b);
}

const char * library_sort_name(lib_sort_t s)
{
    static const char * const names[LIB_SORT_COUNT] = { "title", "artist-title", "artist-year", "year", "newest" };
    return (unsigned)s < LIB_SORT_COUNT ? names[s] : "?";
}

const char * library_folder_type_name(folder_type_t t)
{
    static const char * const names[] = { "plain", "album", "collection", "artist", "library" };
    return (unsigned)t < 5 ? names[t] : "?";
}

/* ------------------------------------------------------------- building */

typedef struct {
    library_t * lib;
    /* per-track scratch, freed after grouping */
    char ** album_tag;         /* the raw album tag, "" if none   */
    char ** album_artist;      /* album artist tag, "" if none    */
    char ** folder;
    int   * year;
} build_t;

static bool take_song(const mpd_song_info_t * s, void * ctx)
{
    build_t * b = ctx;
    library_t * lib = b->lib;
    int i = lib->ntracks;
    lib->tracks = grow(lib->tracks, i, sizeof(*lib->tracks));
    b->album_tag = grow(b->album_tag, i, sizeof(char *));
    b->album_artist = grow(b->album_artist, i, sizeof(char *));
    b->folder = grow(b->folder, i, sizeof(char *));
    b->year = grow(b->year, i, sizeof(int));

    lib_track_t * t = &lib->tracks[i];
    memset(t, 0, sizeof(*t));
    t->uri = xstrdup(s->uri);
    t->title = xstrdup(s->title);
    t->artist = xstrdup(s->artist);
    t->track = s->track;
    t->disc = s->disc;
    t->duration = s->duration;
    t->mtime = s->mtime;
    t->added = s->added;
    t->album = -1;

    char f[1024];
    folder_of(s->uri, f, sizeof(f));
    b->album_tag[i] = xstrdup(s->album);
    b->album_artist[i] = xstrdup(s->album_artist);
    b->folder[i] = xstrdup(f);
    b->year[i] = year_of(s->original_date, s->date);
    lib->ntracks++;
    return true;
}

/* Album key for track i, written to `key`. See library.h rules. */
typedef struct { char * key; int track; } keyed_t;

static int keyed_cmp(const void * pa, const void * pb)
{
    const keyed_t * a = pa, * b = pb;
    int c = strcmp(a->key, b->key);
    return c ? c : (a->track - b->track);        /* stable: MPD order within an album */
}

static void group_albums(build_t * b)
{
    library_t * lib = b->lib;
    int n = lib->ntracks;
    if(n == 0) return;

    /* 1. within one folder, do the tracks of one album title share one
     *    artist? (decides the key of tagged albums with no album artist:
     *    a compilation has several, a single in a folder of singles one) */
    keyed_t * byfolder = malloc((size_t)n * sizeof(keyed_t));
    if(!byfolder) abort();
    for(int i = 0; i < n; i++) {
        char * k = NULL;
        if(asprintf(&k, "%s\x1f%s", b->folder[i], b->album_tag[i]) < 0 || !k) abort();
        byfolder[i].key = k;
        byfolder[i].track = i;
    }
    qsort(byfolder, (size_t)n, sizeof(keyed_t), keyed_cmp);
    bool * folder_mixed = calloc((size_t)n, sizeof(bool));      /* indexed by track */
    if(!folder_mixed) abort();
    for(int s = 0; s < n;) {
        int e = s;
        bool mixed = false;
        const char * first = lib->tracks[byfolder[s].track].artist;
        while(e < n && strcmp(byfolder[e].key, byfolder[s].key) == 0) {
            if(strcasecmp(lib->tracks[byfolder[e].track].artist, first) != 0) mixed = true;
            e++;
        }
        for(int k = s; k < e; k++) folder_mixed[byfolder[k].track] = mixed;
        s = e;
    }
    for(int i = 0; i < n; i++) free(byfolder[i].key);
    free(byfolder);

    /* 2. the key */
    keyed_t * keyed = malloc((size_t)n * sizeof(keyed_t));
    if(!keyed) abort();
    for(int i = 0; i < n; i++) {
        char key[2400];
        const lib_track_t * t = &lib->tracks[i];
        if(!b->album_tag[i][0])
            snprintf(key, sizeof(key), "F\x1f%s", b->folder[i]);                        /* folder = album */
        else if(b->album_artist[i][0])
            snprintf(key, sizeof(key), "A\x1f%s\x1f%s", b->album_artist[i], b->album_tag[i]);
        else if(!folder_mixed[i])
            snprintf(key, sizeof(key), "A\x1f%s\x1f%s", t->artist, b->album_tag[i]);
        else
            snprintf(key, sizeof(key), "V\x1f%s\x1f%s", b->folder[i], b->album_tag[i]);  /* compilation, per folder */
        /* keys compare case-insensitively: "Beatles" and "beatles" are one artist */
        for(char * p = key; *p; p++) *p = (char)tolower((unsigned char)*p);
        keyed[i].key = xstrdup(key);
        keyed[i].track = i;
    }
    free(folder_mixed);
    qsort(keyed, (size_t)n, sizeof(keyed_t), keyed_cmp);

    /* 3. one album per key run; albums numbered in the order their first
     *    track lies in the library (folder order), not key order */
    int * first_of_run = malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    if(!first_of_run) abort();
    int runs = 0;
    for(int s = 0; s < n;) {
        int e = s;
        while(e < n && strcmp(keyed[e].key, keyed[s].key) == 0) e++;
        first_of_run[runs++] = s;
        s = e;
    }
    /* order runs by their first track index */
    for(int i = 1; i < runs; i++) {                 /* insertion sort: runs are few vs tracks */
        int v = first_of_run[i], j = i - 1;
        while(j >= 0 && keyed[first_of_run[j]].track > keyed[v].track) { first_of_run[j + 1] = first_of_run[j]; j--; }
        first_of_run[j + 1] = v;
    }

    lib->albums = calloc((size_t)runs, sizeof(lib_album_t));
    if(!lib->albums) abort();
    lib->nalbums = runs;
    for(int r = 0; r < runs; r++) {
        int s = first_of_run[r], e = s;
        while(e < n && strcmp(keyed[e].key, keyed[s].key) == 0) e++;
        lib_album_t * a = &lib->albums[r];
        int fi = keyed[s].track;                     /* first track in library order (keyed_cmp keeps it) */
        const lib_track_t * ft = &lib->tracks[fi];
        a->untagged = !b->album_tag[fi][0];
        a->various = keyed[s].key[0] == 'v';
        a->title = xstrdup(a->untagged ? base_name(b->folder[fi]) : b->album_tag[fi]);
        a->artist = xstrdup(a->untagged ? "" : (b->album_artist[fi][0] ? b->album_artist[fi] : (a->various ? "" : ft->artist)));
        a->folder = xstrdup(b->folder[fi]);
        a->first_uri = xstrdup(ft->uri);
        a->first_track = fi;
        a->track_count = e - s;
        uint64_t h = 1469598103934665603ull;
        for(int k = s; k < e; k++) {
            lib_track_t * t = &lib->tracks[keyed[k].track];
            t->album = r;
            if(!a->year && b->year[keyed[k].track]) a->year = b->year[keyed[k].track];
            long when = t->added ? t->added : t->mtime;
            if(when > a->newest) a->newest = when;
            char num[32];
            snprintf(num, sizeof(num), "\x1f%ld", t->mtime);
            h = fnv_str(fnv_str(h, t->uri), num);
        }
        a->fingerprint = h;
    }
    for(int i = 0; i < n; i++) free(keyed[i].key);
    free(keyed);
    free(first_of_run);
}

static int artist_cmp(const void * pa, const void * pb)
{
    return library_name_cmp(((const lib_artist_t *)pa)->name, ((const lib_artist_t *)pb)->name);
}

static void build_artists(library_t * lib)
{
    /* album artists, plus track artists of albums with none ("various") */
    int cap = 0;
    for(int i = 0; i < lib->nalbums; i++) {
        const lib_album_t * a = &lib->albums[i];
        if(!a->artist[0]) continue;
        int k;
        for(k = 0; k < lib->nartists; k++) if(strcasecmp(lib->artists[k].name, a->artist) == 0) break;
        if(k == lib->nartists) {
            if(lib->nartists == cap) { cap = cap ? cap * 2 : 64; lib->artists = realloc(lib->artists, (size_t)cap * sizeof(lib_artist_t)); if(!lib->artists) abort(); }
            lib->artists[k].name = xstrdup(a->artist);
            lib->artists[k].album_count = 0;
            lib->artists[k].track_count = 0;
            lib->nartists++;
        }
        lib->artists[k].album_count++;
        lib->artists[k].track_count += a->track_count;
    }
    if(lib->nartists) qsort(lib->artists, (size_t)lib->nartists, sizeof(lib_artist_t), artist_cmp);
}

/* ---- folder types ---- */

typedef struct {
    char * uri;
    int parent;                /* index of the parent node, -1 for the root  */
    int tracks;                /* direct                                      */
    int music_subfolders;      /* direct subfolders that hold music below     */
    int album_subfolders;      /* ... typed FOLDER_ALBUM                      */
    int children_album;        /* the one album of all album subfolders, -2 = several, -1 = none */
    int artist_subfolders;     /* ... typed FOLDER_ARTIST                     */
    int album;                 /* album of the direct tracks, -2 = several    */
    const char * artist;       /* artist of the direct tracks, "" = several   */
    int distinct_artists;      /* capped at COLLECTION_MIN_ARTISTS            */
    folder_type_t type;
} fnode_t;

static int fnode_key_cmp(const void * k, const void * e) { return strcmp((const char *)k, ((const fnode_t *)e)->uri); }
static int str_ptr_cmp(const void * a, const void * b) { return strcmp(*(char * const *)a, *(char * const *)b); }

/* Folders typed from the tracks: every folder on the path of every track
 * is a node (the mount root included). Children sort after their parents,
 * so a reverse walk sees every child before its parent. Costs are
 * n log n in tracks and folders, so a 100,000-track card is fine. */
static void build_folders(library_t * lib, char ** track_folder)
{
    int nt = lib->ntracks;
    /* 1. unique folder URIs, with all their ancestors */
    int cap = nt * 2 + 1, n = 0;
    char ** names = malloc((size_t)cap * sizeof(char *));
    if(!names) abort();
    for(int i = 0; i < nt; i++) names[n++] = xstrdup(track_folder[i]);
    qsort(names, (size_t)n, sizeof(char *), str_ptr_cmp);
    int u = 0;
    for(int i = 0; i < n; i++) { if(u && strcmp(names[u - 1], names[i]) == 0) free(names[i]); else names[u++] = names[i]; }
    n = u;
    /* ancestors */
    for(int i = 0; i < n; i++) {
        char f[1024];
        snprintf(f, sizeof(f), "%s", names[i]);
        for(;;) {
            char * slash = strrchr(f, '/');
            if(slash) *slash = 0; else f[0] = 0;
            bool have = false;
            for(int k = 0; k < n && !have; k++) have = strcmp(names[k], f) == 0;     /* ancestors are few */
            if(!have) {
                if(n == cap) { cap *= 2; names = realloc(names, (size_t)cap * sizeof(char *)); if(!names) abort(); }
                names[n++] = xstrdup(f);
            }
            if(!f[0]) break;
        }
    }
    qsort(names, (size_t)n, sizeof(char *), str_ptr_cmp);
    u = 0;
    for(int i = 0; i < n; i++) { if(u && strcmp(names[u - 1], names[i]) == 0) free(names[i]); else names[u++] = names[i]; }
    n = u;

    fnode_t * nodes = calloc((size_t)(n > 0 ? n : 1), sizeof(fnode_t));
    if(!nodes) abort();
    for(int i = 0; i < n; i++) { nodes[i].uri = names[i]; nodes[i].album = -1; nodes[i].children_album = -1; nodes[i].artist = NULL; nodes[i].parent = -1; }
    free(names);
    /* 2. parents */
    for(int i = 0; i < n; i++) {
        if(!nodes[i].uri[0]) continue;
        char f[1024];
        snprintf(f, sizeof(f), "%s", nodes[i].uri);
        char * slash = strrchr(f, '/');
        if(slash) *slash = 0; else f[0] = 0;
        fnode_t * p = bsearch(f, nodes, (size_t)n, sizeof(fnode_t), fnode_key_cmp);
        nodes[i].parent = p ? (int)(p - nodes) : -1;
    }
    /* 3. direct tracks */
    for(int i = 0; i < nt; i++) {
        fnode_t * nd = bsearch(track_folder[i], nodes, (size_t)n, sizeof(fnode_t), fnode_key_cmp);
        if(!nd) continue;
        nd->tracks++;
        int a = lib->tracks[i].album;
        if(nd->album == -1) nd->album = a; else if(nd->album != a) nd->album = -2;
        const char * ar = lib->tracks[i].artist;
        if(!nd->artist) { nd->artist = ar; nd->distinct_artists = 1; }
        else if(nd->distinct_artists < COLLECTION_MIN_ARTISTS && strcasecmp(nd->artist, ar) != 0) {
            /* count distinct artists among the first few seen: enough to
             * tell a compilation folder from an album */
            nd->distinct_artists++;
            nd->artist = ar;
        }
    }
    /* 4. type, children first */
    for(int k = n - 1; k >= 0; k--) {
        fnode_t * nd = &nodes[k];
        if(nd->tracks > 0 && nd->music_subfolders == 0 && nd->album >= 0)
            nd->type = FOLDER_ALBUM;
        else if(nd->tracks == 0 && nd->music_subfolders >= 2 && nd->album_subfolders == nd->music_subfolders && nd->children_album >= 0) {
            /* CD1/CD2 folders of one album: the folder above them is the album */
            nd->type = FOLDER_ALBUM;
            nd->album = nd->children_album;
        }
        else if(nd->tracks >= COLLECTION_MIN_TRACKS && nd->distinct_artists >= COLLECTION_MIN_ARTISTS)
            nd->type = FOLDER_COLLECTION;
        else if(nd->tracks == 0 && nd->album_subfolders >= ARTIST_MIN_ALBUMS && nd->album_subfolders * 3 >= nd->music_subfolders * 2)
            nd->type = FOLDER_ARTIST;
        else if(nd->tracks == 0 && nd->artist_subfolders >= LIBRARY_MIN_ARTISTS && nd->artist_subfolders * 2 >= nd->music_subfolders)
            nd->type = FOLDER_LIBRARY;
        else
            nd->type = FOLDER_PLAIN;
        if(nd->parent >= 0) {
            fnode_t * p = &nodes[nd->parent];
            p->music_subfolders++;
            if(nd->type == FOLDER_ALBUM) {
                p->album_subfolders++;
                if(p->children_album == -1) p->children_album = nd->album;
                else if(p->children_album != nd->album) p->children_album = -2;
            }
            if(nd->type == FOLDER_ARTIST) p->artist_subfolders++;
        }
    }

    lib->folders = calloc((size_t)(n > 0 ? n : 1), sizeof(lib_folder_t));
    if(!lib->folders) abort();
    lib->nfolders = n;
    for(int k = 0; k < n; k++) {
        lib->folders[k].uri = nodes[k].uri;
        lib->folders[k].type = nodes[k].type;
        lib->folders[k].album = nodes[k].type == FOLDER_ALBUM ? nodes[k].album : -1;
        lib->folders[k].tracks = nodes[k].tracks;
        lib->folders[k].subfolders = nodes[k].music_subfolders;
    }
    /* an album whose tracks all lie in collection folders is a single (or a
     * few tracks) from a folder of singles, not an album in its own right */
    for(int a = 0; a < lib->nalbums; a++) lib->albums[a].in_collection = lib->albums[a].track_count > 0;
    for(int i = 0; i < nt; i++) {
        int a = lib->tracks[i].album;
        if(a < 0 || !lib->albums[a].in_collection) continue;
        fnode_t * nd = bsearch(track_folder[i], nodes, (size_t)n, sizeof(fnode_t), fnode_key_cmp);
        if(!nd || nd->type != FOLDER_COLLECTION) lib->albums[a].in_collection = false;
    }
    free(nodes);
}

bool library_build(library_t * lib, const char * root)
{
    memset(lib, 0, sizeof(*lib));
    lib->root = xstrdup(root);
    build_t b = { .lib = lib };
    bool ok = mpd_listallinfo(root, take_song, &b);
    if(ok) {
        group_albums(&b);
        build_artists(lib);
        build_folders(lib, b.folder);
    }
    for(int i = 0; i < lib->ntracks; i++) { free(b.album_tag[i]); free(b.album_artist[i]); free(b.folder[i]); }
    free(b.album_tag); free(b.album_artist); free(b.folder); free(b.year);
    if(!ok) library_free(lib);
    return ok;
}

void library_free(library_t * lib)
{
    for(int i = 0; i < lib->ntracks; i++) { free(lib->tracks[i].uri); free(lib->tracks[i].title); free(lib->tracks[i].artist); }
    for(int i = 0; i < lib->nalbums; i++) { free(lib->albums[i].title); free(lib->albums[i].artist); free(lib->albums[i].folder); free(lib->albums[i].first_uri); }
    for(int i = 0; i < lib->nartists; i++) free(lib->artists[i].name);
    for(int i = 0; i < lib->nfolders; i++) free(lib->folders[i].uri);
    free(lib->tracks); free(lib->albums); free(lib->artists); free(lib->folders); free(lib->root);
    memset(lib, 0, sizeof(*lib));
}

/* ---------------------------------------------------------------- sorts */

static const library_t * sort_lib;
static lib_sort_t sort_kind;

static bool lacks(const lib_album_t * a, lib_sort_t s)
{
    switch(s) {
        case LIB_SORT_ARTIST_TITLE: case LIB_SORT_ARTIST_YEAR: return !a->artist[0];
        case LIB_SORT_YEAR:   return a->year == 0;
        case LIB_SORT_NEWEST: return a->newest == 0;
        default:              return a->untagged;          /* title sort: no album tag */
    }
}

static int album_order_cmp(const void * pa, const void * pb)
{
    const lib_album_t * a = &sort_lib->albums[*(const int *)pa], * b = &sort_lib->albums[*(const int *)pb];
    bool la = lacks(a, sort_kind), lb = lacks(b, sort_kind);
    if(la != lb) return la ? 1 : -1;
    if(la) return strcmp(a->folder, b->folder);            /* the end section: folder order */
    int c = 0;
    switch(sort_kind) {
        case LIB_SORT_TITLE:        c = library_name_cmp(a->title, b->title); break;
        case LIB_SORT_ARTIST_TITLE: c = library_name_cmp(a->artist, b->artist); if(!c) c = library_name_cmp(a->title, b->title); break;
        case LIB_SORT_ARTIST_YEAR:  c = library_name_cmp(a->artist, b->artist); if(!c) c = (a->year > b->year) - (a->year < b->year); if(!c) c = library_name_cmp(a->title, b->title); break;
        case LIB_SORT_YEAR:         c = (a->year > b->year) - (a->year < b->year); if(!c) c = library_name_cmp(a->artist, b->artist); if(!c) c = library_name_cmp(a->title, b->title); break;
        case LIB_SORT_NEWEST:       c = (a->newest < b->newest) - (a->newest > b->newest); break;
        default: break;
    }
    if(!c) c = strcmp(a->folder, b->folder);
    return c;
}

void library_sort_albums(const library_t * lib, lib_sort_t sort, int * order)
{
    for(int i = 0; i < lib->nalbums; i++) order[i] = i;
    sort_lib = lib;
    sort_kind = sort;
    qsort(order, (size_t)lib->nalbums, sizeof(int), album_order_cmp);
}

int library_albums_of_artist(const library_t * lib, const char * artist, int * out, int max)
{
    int * order = malloc((size_t)(lib->nalbums ? lib->nalbums : 1) * sizeof(int));
    if(!order) return 0;
    library_sort_albums(lib, LIB_SORT_ARTIST_YEAR, order);
    int n = 0;
    for(int i = 0; i < lib->nalbums && n < max; i++)
        if(strcasecmp(lib->albums[order[i]].artist, artist) == 0) out[n++] = order[i];
    free(order);
    return n;
}

const lib_folder_t * library_folder(const library_t * lib, const char * uri)
{
    for(int i = 0; i < lib->nfolders; i++) if(strcmp(lib->folders[i].uri, uri) == 0) return &lib->folders[i];
    return NULL;
}

/* ----------------------------------------------------------- index file */

/* Fields are tab-separated; a tab or newline inside a tag becomes a space. */
static void put_field(FILE * f, const char * s)
{
    for(; *s; s++) fputc((*s == '\t' || *s == '\n' || *s == '\r') ? ' ' : *s, f);
    fputc('\t', f);
}

bool library_save(const library_t * lib, const char * path)
{
    char tmp[1200];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE * f = fopen(tmp, "w");
    if(!f) return false;
    fprintf(f, "simpleton-library\t2\n");
    fprintf(f, "root\t%s\n", lib->root);
    fprintf(f, "albums\t%d\n", lib->nalbums);
    for(int i = 0; i < lib->nalbums; i++) {
        const lib_album_t * a = &lib->albums[i];
        fputs("A\t", f);
        put_field(f, a->title); put_field(f, a->artist); put_field(f, a->folder); put_field(f, a->first_uri);
        fprintf(f, "%d\t%ld\t%d\t%d\t%d\t%d\t%016llx\t%06x\t%d\t%u\t%d\n", a->year, a->newest, a->first_track, a->track_count,
                a->untagged, a->various, (unsigned long long)a->fingerprint, a->colour, a->has_art, a->thumb_sizes, a->in_collection);
    }
    fprintf(f, "tracks\t%d\n", lib->ntracks);
    for(int i = 0; i < lib->ntracks; i++) {
        const lib_track_t * t = &lib->tracks[i];
        fputs("T\t", f);
        put_field(f, t->uri); put_field(f, t->title); put_field(f, t->artist);
        fprintf(f, "%d\t%d\t%.3f\t%ld\t%ld\t%d\n", t->track, t->disc, (double)t->duration, t->mtime, t->added, t->album);
    }
    fprintf(f, "artists\t%d\n", lib->nartists);
    for(int i = 0; i < lib->nartists; i++) {
        fputs("R\t", f);
        put_field(f, lib->artists[i].name);
        fprintf(f, "%d\t%d\n", lib->artists[i].album_count, lib->artists[i].track_count);
    }
    fprintf(f, "folders\t%d\n", lib->nfolders);
    for(int i = 0; i < lib->nfolders; i++) {
        fputs("F\t", f);
        put_field(f, lib->folders[i].uri);
        fprintf(f, "%d\t%d\t%d\t%d\n", (int)lib->folders[i].type, lib->folders[i].album, lib->folders[i].tracks, lib->folders[i].subfolders);
    }
    fprintf(f, "end\n");
    bool ok = fflush(f) == 0 && !ferror(f);
    if(fclose(f) != 0) ok = false;
    if(ok && rename(tmp, path) != 0) ok = false;
    if(!ok) remove(tmp);
    return ok;
}

/* Split a line into tab fields in place; returns the count. */
static int split(char * line, char ** fields, int max)
{
    int n = 0;
    char * p = line;
    line[strcspn(line, "\r\n")] = 0;
    while(n < max) {
        fields[n++] = p;
        char * tab = strchr(p, '\t');
        if(!tab) break;
        *tab = 0;
        p = tab + 1;
    }
    return n;
}

bool library_load(library_t * lib, const char * path)
{
    memset(lib, 0, sizeof(*lib));
    FILE * f = fopen(path, "r");
    if(!f) return false;
    size_t cap = 8192;
    char * line = malloc(cap);
    if(!line) { fclose(f); return false; }
    char * fl[24];
    bool ok = false;
    int na = -1, nt = -1, nr = -1, nf = -1, ia = 0, it = 0, ir = 0, iff = 0;
    while(fgets(line, (int)cap, f)) {
        int n = split(line, fl, 24);
        if(n < 1) continue;
        if(strcmp(fl[0], "simpleton-library") == 0) { if(n < 2 || atoi(fl[1]) != 2) break; }   /* an older file is rebuilt */
        else if(strcmp(fl[0], "root") == 0 && n >= 2) lib->root = xstrdup(fl[1]);
        else if(strcmp(fl[0], "albums") == 0 && n >= 2) { na = atoi(fl[1]); lib->albums = calloc((size_t)(na > 0 ? na : 1), sizeof(lib_album_t)); if(!lib->albums) break; }
        else if(strcmp(fl[0], "tracks") == 0 && n >= 2) { nt = atoi(fl[1]); lib->tracks = calloc((size_t)(nt > 0 ? nt : 1), sizeof(lib_track_t)); if(!lib->tracks) break; }
        else if(strcmp(fl[0], "artists") == 0 && n >= 2) { nr = atoi(fl[1]); lib->artists = calloc((size_t)(nr > 0 ? nr : 1), sizeof(lib_artist_t)); if(!lib->artists) break; }
        else if(strcmp(fl[0], "folders") == 0 && n >= 2) { nf = atoi(fl[1]); lib->folders = calloc((size_t)(nf > 0 ? nf : 1), sizeof(lib_folder_t)); if(!lib->folders) break; }
        else if(strcmp(fl[0], "A") == 0 && n >= 15 && ia < na) {
            lib_album_t * a = &lib->albums[ia++];
            a->title = xstrdup(fl[1]); a->artist = xstrdup(fl[2]); a->folder = xstrdup(fl[3]); a->first_uri = xstrdup(fl[4]);
            a->year = atoi(fl[5]); a->newest = atol(fl[6]); a->first_track = atoi(fl[7]); a->track_count = atoi(fl[8]);
            a->untagged = atoi(fl[9]) != 0; a->various = atoi(fl[10]) != 0;
            a->fingerprint = strtoull(fl[11], NULL, 16); a->colour = (uint32_t)strtoul(fl[12], NULL, 16);
            a->has_art = atoi(fl[13]) != 0; a->thumb_sizes = (uint8_t)atoi(fl[14]);
            a->in_collection = n >= 16 && atoi(fl[15]) != 0;
            lib->nalbums = ia;
        }
        else if(strcmp(fl[0], "T") == 0 && n >= 10 && it < nt) {
            lib_track_t * t = &lib->tracks[it++];
            t->uri = xstrdup(fl[1]); t->title = xstrdup(fl[2]); t->artist = xstrdup(fl[3]);
            t->track = atoi(fl[4]); t->disc = atoi(fl[5]); t->duration = (float)atof(fl[6]); t->mtime = atol(fl[7]); t->added = atol(fl[8]); t->album = atoi(fl[9]);
            if(t->album < 0 || t->album >= na) t->album = -1;
            lib->ntracks = it;
        }
        else if(strcmp(fl[0], "R") == 0 && n >= 4 && ir < nr) {
            lib_artist_t * r = &lib->artists[ir++];
            r->name = xstrdup(fl[1]); r->album_count = atoi(fl[2]); r->track_count = atoi(fl[3]);
            lib->nartists = ir;
        }
        else if(strcmp(fl[0], "F") == 0 && n >= 6 && iff < nf) {
            lib_folder_t * fo = &lib->folders[iff++];
            fo->uri = xstrdup(fl[1]); fo->type = (folder_type_t)atoi(fl[2]); fo->album = atoi(fl[3]); fo->tracks = atoi(fl[4]); fo->subfolders = atoi(fl[5]);
            if(fo->type > FOLDER_LIBRARY) fo->type = FOLDER_PLAIN;
            lib->nfolders = iff;
        }
        else if(strcmp(fl[0], "end") == 0) { ok = ia == na && it == nt && ir == nr && iff == nf; break; }
    }
    free(line);
    fclose(f);
    if(!lib->root) lib->root = xstrdup("");
    if(!ok) library_free(lib);
    return ok;
}
