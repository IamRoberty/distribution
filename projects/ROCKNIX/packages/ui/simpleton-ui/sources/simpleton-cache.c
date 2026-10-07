/*
 * simpleton-cache - builds and inspects the library cache (0.12, 4 Oct 2026).
 *
 *   simpleton-cache build <card> [-v]      build or refresh the card's cache
 *   simpleton-cache status <card>          what the cache holds
 *   simpleton-cache albums <card> [sort]   albums in a picker sort
 *                                          (title, artist-title, artist-year, year, newest)
 *   simpleton-cache folders <card>         folder types
 *
 * <card> is the MPD mount name (card-<serial>). Runs at idle priority so it
 * can never get in the way of playback. Shares its sources with the UI
 * (library.c, cache.c, art.c, mpdc.c, placeholder.c): no LVGL in here.
 */
#include "cache.h"
#include "library.h"
#include "mpdc.h"
#include "strings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

static void go_idle(void)
{
    setpriority(PRIO_PROCESS, 0, 19);
#ifdef SYS_ioprio_set
    /* IOPRIO_WHO_PROCESS = 1, class IDLE = 3 << 13 */
    syscall(SYS_ioprio_set, 1, 0, 3 << 13);
#endif
}

static void usage(void)
{
    fputs("usage: simpleton-cache build <card> [-v] | status <card> | albums <card> [sort] | folders <card>\n", stderr);
}

static bool load_or_build(const char * card, library_t * lib)
{
    char idx[1300];
    cache_index_path(card, idx, sizeof(idx));
    if(library_load(lib, idx)) return true;
    fprintf(stderr, "simpleton-cache: no index for %s yet, asking MPD\n", card);
    return library_build(lib, card);
}

int main(int argc, char ** argv)
{
    if(argc < 3) { usage(); return 2; }
    const char * cmd = argv[1], * card = argv[2];
    strings_init();

    if(strcmp(cmd, "build") == 0) {
        go_idle();
        bool verbose = argc > 3 && strcmp(argv[3], "-v") == 0;
        return cache_build(card, verbose) ? 0 : 1;
    }

    library_t lib;
    if(!load_or_build(card, &lib)) { fprintf(stderr, "simpleton-cache: cannot read the library of %s\n", card); return 1; }

    if(strcmp(cmd, "status") == 0) {
        int art = 0, thumbs = 0, singles = 0;
        for(int i = 0; i < lib.nalbums; i++) { if(lib.albums[i].has_art) art++; if(lib.albums[i].thumb_sizes == (1u << LIB_THUMB_COUNT) - 1) thumbs++; if(lib.albums[i].in_collection) singles++; }
        int types[5] = { 0 };
        for(int i = 0; i < lib.nfolders; i++) types[lib.folders[i].type]++;
        printf("%s: %d tracks, %d albums (%d with art, %d fully thumbnailed, %d singles in collection folders), %d artists, %d folders (%d album, %d collection, %d artist, %d library, %d plain)\n",
               card, lib.ntracks, lib.nalbums, art, thumbs, singles, lib.nartists, lib.nfolders, types[FOLDER_ALBUM], types[FOLDER_COLLECTION], types[FOLDER_ARTIST], types[FOLDER_LIBRARY], types[FOLDER_PLAIN]);
    }
    else if(strcmp(cmd, "albums") == 0) {
        lib_sort_t sort = LIB_SORT_TITLE;
        if(argc > 3) { for(int s = 0; s < LIB_SORT_COUNT; s++) if(strcmp(argv[3], library_sort_name((lib_sort_t)s)) == 0) sort = (lib_sort_t)s; }
        int * order = malloc((size_t)(lib.nalbums ? lib.nalbums : 1) * sizeof(int));
        if(!order) return 1;
        library_sort_albums(&lib, sort, order);
        printf("# %d albums, sort %s\n", lib.nalbums, library_sort_name(sort));
        for(int i = 0; i < lib.nalbums; i++) {
            const lib_album_t * a = &lib.albums[order[i]];
            char key[24];
            cache_album_key(a, key, sizeof(key));
            char year[8] = "    ";
            if(a->year) snprintf(year, sizeof(year), "%4d", a->year);
            printf("%4d  %s  %-30.30s  %-40.40s  %3d tr  %s%s%s  #%06x  %s\n", i + 1, year, a->artist, a->title, a->track_count,
                   a->untagged ? "untagged " : "", a->various ? "various " : "", a->has_art ? "art" : "no-art", a->colour, a->folder);
            if(a->in_collection) printf("      (in a collection folder: left out of the Albums grid by default)\n");
        }
        free(order);
    }
    else if(strcmp(cmd, "folders") == 0) {
        for(int i = 0; i < lib.nfolders; i++) {
            const lib_folder_t * f = &lib.folders[i];
            printf("%-10s %3d tracks %3d subfolders  %s\n", library_folder_type_name(f->type), f->tracks, f->subfolders, f->uri[0] ? f->uri : "(root)");
        }
    }
    else { usage(); library_free(&lib); return 2; }
    library_free(&lib);
    return 0;
}
