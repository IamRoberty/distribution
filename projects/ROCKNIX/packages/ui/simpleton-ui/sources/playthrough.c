/*
 * SimpletonOS UI - play through folders (implementation). See playthrough.h.
 */
#include "playthrough.h"
#include "mpdc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SETTING_FILE "/storage/.config/simpleton/play_through"
#define MAX_CLIMB    32      /* folder levels; also the recursion bound going down */

/* The (file, queue length) we last appended for, so one trigger never
 * appends twice if MPD's notices arrive in an odd order. */
static char last_file[1024];
static int  last_len = -1;

bool playthrough_enabled(void)
{
    FILE * f = fopen(SETTING_FILE, "r");
    if(!f) return true;                          /* default: on */
    char v[16] = "";
    if(!fgets(v, sizeof(v), f)) v[0] = 0;
    fclose(f);
    return !(v[0] == '0' || strncmp(v, "off", 3) == 0 || strncmp(v, "false", 5) == 0);
}

/* "A/B/track.flac" -> "A/B"; "track.flac" -> "". Caller frees. */
static char * dirname_of(const char * uri)
{
    const char * slash = strrchr(uri, '/');
    size_t n = slash ? (size_t)(slash - uri) : 0;
    char * out = malloc(n + 1);
    if(!out) return NULL;
    memcpy(out, uri, n);
    out[n] = 0;
    return out;
}

/* The tracks of folder `dir` as the browser lists it; when it has none,
 * the tracks of the first sub-folder (depth first) that has any. Fills
 * `uris` (caller frees each and the array). */
static bool first_tracks(const char * dir, int depth, char *** uris, int * count)
{
    if(depth > MAX_CLIMB) return false;
    mpd_listing_t l;
    if(!mpd_lsinfo_expanded(dir, &l)) return false;

    int files = 0;
    for(int i = 0; i < l.count; i++) if(l.items[i].kind == MPD_ENTRY_FILE) files++;
    if(files) {
        char ** v = calloc((size_t)files, sizeof(char *));
        if(!v) { mpd_listing_free(&l); return false; }
        int n = 0;
        for(int i = 0; i < l.count; i++)
            if(l.items[i].kind == MPD_ENTRY_FILE) v[n++] = strdup(l.items[i].uri);
        *uris = v;
        *count = n;
        mpd_listing_free(&l);
        return true;
    }
    bool found = false;
    for(int i = 0; i < l.count && !found; i++)
        if(l.items[i].kind == MPD_ENTRY_DIR) found = first_tracks(l.items[i].uri, depth + 1, uris, count);
    mpd_listing_free(&l);
    return found;
}

/* HiBy's walk: the tracks of the next folder after the one holding `file`. */
static bool next_folder_tracks(const char * file, char *** uris, int * count)
{
    char * dir = dirname_of(file);
    if(!dir) return false;
    /* a track inside a container (Album/disc.iso/track) belongs to the
     * folder holding the container */
    while(dir[0] && mpd_is_container_name(dir)) {
        char * up = dirname_of(dir);
        free(dir);
        dir = up;
        if(!dir) return false;
    }
    if(!dir[0]) { free(dir); return false; }     /* loose track in the root: nothing above */

    bool found = false;
    for(int climb = 0; climb < MAX_CLIMB && !found; climb++) {
        char * parent = dirname_of(dir);
        if(!parent) break;
        mpd_listing_t l;
        if(mpd_lsinfo(parent, &l)) {
            bool after = false;
            for(int i = 0; i < l.count && !found; i++) {
                const mpd_entry_t * e = &l.items[i];
                if(e->kind != MPD_ENTRY_DIR) continue;
                if(!after) { if(strcmp(e->uri, dir) == 0) after = true; continue; }
                if(mpd_is_container_name(e->uri)) continue;   /* a disc image is part of its folder, not a folder */
                found = first_tracks(e->uri, 0, uris, count);
            }
            mpd_listing_free(&l);
        }
        bool at_root = parent[0] == 0;
        free(dir);
        dir = parent;
        if(at_root) break;
    }
    free(dir);
    return found;
}

static void check(void)
{
    if(!playthrough_enabled()) return;
    mpd_status_t st;
    if(!mpd_status(&st)) return;
    if(strcmp(st.state, "play") != 0 && strcmp(st.state, "pause") != 0) return;
    if(!st.file[0] || st.nextsong >= 0) return;   /* MPD already has somewhere to go */
    if(st.repeat || st.single) return;            /* repeat / repeat-one: HiBy disables it too */
    if(strcmp(st.file, last_file) == 0 && st.playlistlength == last_len) return;

    char ** uris = NULL;
    int n = 0;
    if(!next_folder_tracks(st.file, &uris, &n)) {
        /* end of the library: remember so this isn't re-walked on every notice */
        snprintf(last_file, sizeof(last_file), "%s", st.file);
        last_len = st.playlistlength;
        return;
    }
    bool ok = mpd_add_uris(uris, n);
    if(ok) {
        snprintf(last_file, sizeof(last_file), "%s", st.file);
        last_len = st.playlistlength;
        char * folder = dirname_of(uris[0]);
        fprintf(stderr, "simpleton-ui: play-through: queued %d tracks from %s\n", n, folder ? folder : "?");
        free(folder);
    }
    for(int i = 0; i < n; i++) free(uris[i]);
    free(uris);
}

void playthrough_mpd_changed(unsigned what)
{
    if(what & MPD_CHANGED_DATABASE) last_len = -1;    /* the library grew: look again */
    if(what & (MPD_CHANGED_PLAYER | MPD_CHANGED_PLAYLIST | MPD_CHANGED_OPTIONS | MPD_CHANGED_DATABASE)) check();
}
