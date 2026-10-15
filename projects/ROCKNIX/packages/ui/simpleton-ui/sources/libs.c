/*
 * SimpletonOS UI - the mounted cards' library indexes (implementation).
 * See libs.h. The loading code is grid.c's of 0.16h, shared since 0.19.
 */
#include "libs.h"
#include "cache.h"
#include "mpdc.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_PENDING 8

static lib_source_t sources[LIBS_MAX];
static int          nsources;
static bool         loaded;
static unsigned     generation;
static char         pending_idx[MAX_PENDING][320];
static int          npending;

bool libs_loaded(void) { return loaded; }
int  libs_count(void) { return nsources; }
const lib_source_t * libs_get(int i) { return (i >= 0 && i < nsources) ? &sources[i] : NULL; }
unsigned libs_generation(void) { return generation; }

bool libs_load(void)
{
    for(int s = 0; s < nsources; s++) library_free(&sources[s].lib);
    nsources = 0;
    npending = 0;
    loaded = false;
    generation++;

    mpd_listing_t root;
    if(!mpd_lsinfo("", &root)) return false;
    loaded = true;
    for(int i = 0; i < root.count && nsources < LIBS_MAX; i++) {
        const mpd_entry_t * e = &root.items[i];
        if(e->kind != MPD_ENTRY_DIR || strncmp(e->uri, "card-", 5) != 0 || strchr(e->uri, '/')) continue;
        lib_source_t * s = &sources[nsources];
        snprintf(s->card, sizeof(s->card), "%s", e->uri);
        cache_index_path(s->card, s->idx_path, sizeof(s->idx_path));
        struct stat st;
        if(stat(s->idx_path, &st) != 0) {
            if(npending < MAX_PENDING) snprintf(pending_idx[npending++], sizeof(pending_idx[0]), "%s", s->idx_path);
            continue;
        }
        if(!library_load(&s->lib, s->idx_path)) continue;
        s->idx_mtime = st.st_mtime;
        nsources++;
    }
    mpd_listing_free(&root);
    int albums = 0, tracks = 0;
    for(int s = 0; s < nsources; s++) { albums += sources[s].lib.nalbums; tracks += sources[s].lib.ntracks; }
    fprintf(stderr, "simpleton-ui: library: %d card%s indexed, %d albums, %d tracks%s\n",
            nsources, nsources == 1 ? "" : "s", albums, tracks, npending ? " (a card still without an index)" : "");
    return true;
}

bool libs_changed(void)
{
    for(int s = 0; s < nsources; s++) {
        struct stat st;
        if(stat(sources[s].idx_path, &st) != 0) return true;
        if(st.st_mtime != sources[s].idx_mtime) return true;
    }
    for(int i = 0; i < npending; i++) {
        struct stat st;
        if(stat(pending_idx[i], &st) == 0) return true;
    }
    if(nsources == 0 && npending == 0) {
        mpd_listing_t root;
        if(!mpd_lsinfo("", &root)) return false;
        bool card = false;
        for(int i = 0; i < root.count && !card; i++)
            card = root.items[i].kind == MPD_ENTRY_DIR && strncmp(root.items[i].uri, "card-", 5) == 0 && !strchr(root.items[i].uri, '/');
        mpd_listing_free(&root);
        if(card) return true;
    }
    return false;
}

bool libs_refresh(void)
{
    if(loaded && !libs_changed()) return false;
    libs_load();
    return true;
}

const lib_source_t * libs_find_folder(const char * uri, const lib_folder_t ** folder)
{
    if(!uri || !uri[0]) return NULL;
    for(int s = 0; s < nsources; s++) {
        const lib_folder_t * f = library_folder(&sources[s].lib, uri);
        if(f) { if(folder) *folder = f; return &sources[s]; }
    }
    return NULL;
}
