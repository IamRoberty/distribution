/*
 * SimpletonOS UI - the artists of the mounted cards (implementation).
 * See artists.h.
 */
#include "artists.h"
#include "art.h"
#include "libs.h"
#include "strings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define FEW_TRACKS 5          /* an artist with no album of their own and fewer tracks than this stays out */

static artist_t * artists;
static int        nartists;
static unsigned   lib_gen, generation;

static void artists_free(void)
{
    for(int i = 0; i < nartists; i++) { free(artists[i].name); free(artists[i].albums); free(artists[i].pic); }
    free(artists);
    artists = NULL;
    nartists = 0;
}

static int find(const char * name)
{
    for(int i = 0; i < nartists; i++) if(!artists[i].various && strcasecmp(artists[i].name, name) == 0) return i;
    return -1;
}

static artist_t * add(const char * name, bool various)
{
    artists = realloc(artists, sizeof(artist_t) * (size_t)(nartists + 1));
    if(!artists) abort();
    artist_t * a = &artists[nartists++];
    memset(a, 0, sizeof(*a));
    a->name = strdup(name);
    a->various = various;
    return a;
}

static void add_album(artist_t * a, int src, int album)
{
    a->albums = realloc(a->albums, sizeof(artist_album_t) * (size_t)(a->nalbums + 1));
    if(!a->albums) abort();
    a->albums[a->nalbums].src = src;
    a->albums[a->nalbums].album = album;
    a->nalbums++;
    a->ntracks += libs_get(src)->lib.albums[album].track_count;
}

static const lib_album_t * alb(const artist_album_t * r) { return &libs_get(r->src)->lib.albums[r->album]; }

static int album_cmp(const void * pa, const void * pb)
{
    const lib_album_t * a = alb(pa), * b = alb(pb);
    if(a->year != b->year) { if(!a->year) return 1; if(!b->year) return -1; return a->year < b->year ? -1 : 1; }
    return library_name_cmp(a->title, b->title);
}

static int artist_cmp(const void * pa, const void * pb)
{
    const artist_t * a = pa, * b = pb;
    if(a->various != b->various) return a->various ? 1 : -1;       /* Various Artists last */
    return library_name_cmp(a->name, b->name);
}

/* The folder named after the artist that holds one of their albums: its
 * picture is the artist's. */
static char * find_picture(const artist_t * a)
{
    for(int k = 0; k < a->nalbums; k++) {
        const lib_album_t * al = alb(&a->albums[k]);
        if(!al->folder || !al->folder[0]) continue;
        char parent[1024];
        snprintf(parent, sizeof(parent), "%s", al->folder);
        for(int up = 0; up < 2; up++) {
            char * slash = strrchr(parent, '/');
            if(!slash) break;
            const char * base = slash + 1;
            if(strcasecmp(base, a->name) == 0) {
                char pic[1024];
                if(art_folder_image(parent, pic, sizeof(pic))) return strdup(pic);
                break;
            }
            *slash = 0;
        }
    }
    return NULL;
}

bool artists_refresh(void)
{
    libs_refresh();
    if(artists && lib_gen == libs_generation()) return false;
    artists_free();
    lib_gen = libs_generation();
    generation++;
    int various = -1;
    for(int s = 0; s < libs_count(); s++) {
        const library_t * lib = &libs_get(s)->lib;
        for(int i = 0; i < lib->nalbums; i++) {
            const lib_album_t * al = &lib->albums[i];
            bool va = al->various || strcasecmp(al->artist, "Various Artists") == 0 || strcasecmp(al->artist, "Various") == 0 || strcasecmp(al->artist, "VA") == 0;
            if(va) {
                if(various < 0) { add(T(S_VARIOUS_ARTISTS), true); various = nartists - 1; }
                add_album(&artists[various], s, i);
                continue;
            }
            if(!al->artist[0]) continue;
            int k = find(al->artist);
            if(k < 0) { add(al->artist, false); k = nartists - 1; }
            add_album(&artists[k], s, i);
        }
    }
    /* out: artists with nothing but a few singles */
    int w = 0;
    for(int i = 0; i < nartists; i++) {
        artist_t * a = &artists[i];
        bool own = false;
        for(int k = 0; k < a->nalbums && !own; k++) own = !alb(&a->albums[k])->in_collection;
        if(a->various || own || a->ntracks >= FEW_TRACKS) artists[w++] = *a;
        else { free(a->name); free(a->albums); free(a->pic); }
    }
    nartists = w;
    for(int i = 0; i < nartists; i++) {
        if(artists[i].nalbums > 1) qsort(artists[i].albums, (size_t)artists[i].nalbums, sizeof(artist_album_t), album_cmp);
        artists[i].pic = artists[i].various ? NULL : find_picture(&artists[i]);
    }
    if(nartists > 1) qsort(artists, (size_t)nartists, sizeof(artist_t), artist_cmp);
    int pics = 0;
    for(int i = 0; i < nartists; i++) if(artists[i].pic) pics++;
    fprintf(stderr, "simpleton-ui: artists: %d listed (%d with a picture)%s\n", nartists, pics, various >= 0 ? ", Various Artists last" : "");
    return true;
}

int artists_count(void) { return nartists; }
const artist_t * artists_get(int i) { return (i >= 0 && i < nartists) ? &artists[i] : NULL; }
unsigned artists_generation(void) { return generation; }

int artists_find(const char * name)
{
    if(!name) return -1;
    for(int i = 0; i < nartists; i++) if(strcasecmp(artists[i].name, name) == 0) return i;
    return -1;
}

const char * artists_count_text(int i, bool tracks, char * out, size_t len)
{
    const artist_t * a = artists_get(i);
    out[0] = 0;
    if(!a) return out;
    int n = tracks ? a->ntracks : a->nalbums;
    if(n == 1) snprintf(out, len, "%s", T(tracks ? S_LIST_TRACK_ONE : S_LIST_ALBUM_ONE));
    else { char d[16]; snprintf(d, sizeof(d), "%d", n); ui_strf(out, len, tracks ? S_LIST_TRACKS_COUNT : S_LIST_ALBUMS_COUNT, "n", d, NULL); }
    return out;
}

const char ** artists_track_uris(int i, int * count)
{
    *count = 0;
    const artist_t * a = artists_get(i);
    if(!a) return NULL;
    const char ** out = calloc((size_t)(a->ntracks > 0 ? a->ntracks : 1), sizeof(char *));
    if(!out) return NULL;
    int n = 0;
    for(int k = 0; k < a->nalbums; k++) {
        const library_t * lib = &libs_get(a->albums[k].src)->lib;
        int album = a->albums[k].album;
        /* the album's tracks in disc / track order */
        int * idx = malloc(sizeof(int) * (size_t)(lib->ntracks > 0 ? lib->ntracks : 1));
        int m = 0;
        if(!idx) break;
        for(int t = 0; t < lib->ntracks; t++) if(lib->tracks[t].album == album) idx[m++] = t;
        for(int x = 1; x < m; x++) {       /* insertion sort: an album is short */
            int v = idx[x], y = x - 1;
            while(y >= 0 && (lib->tracks[idx[y]].disc > lib->tracks[v].disc || (lib->tracks[idx[y]].disc == lib->tracks[v].disc && lib->tracks[idx[y]].track > lib->tracks[v].track))) { idx[y + 1] = idx[y]; y--; }
            idx[y + 1] = v;
        }
        for(int x = 0; x < m && n < a->ntracks; x++) out[n++] = lib->tracks[idx[x]].uri;
        free(idx);
    }
    *count = n;
    return out;
}
