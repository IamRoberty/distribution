/*
 * SimpletonOS UI - the library indexes of the mounted cards (0.19, 13 Oct
 * 2026). Moved out of grid.c so every browse view (the album grid, the
 * track lists, the folder views) reads one copy of each card's
 * library.idx and notices together when an index appears or changes.
 *
 * A "source" is one card MPD has mounted (card-<serial>/ at the root) with
 * an index on the system card. Cards mounted but not yet indexed (a scan
 * still running, the copy-back after a boot) are watched, so the views
 * fill in by themselves when the index lands (0.16h).
 */
#ifndef SIMPLETON_LIBS_H
#define SIMPLETON_LIBS_H

#include "library.h"
#include <stdbool.h>
#include <time.h>

#define LIBS_MAX 4

typedef struct {
    library_t lib;
    char      card[64];
    char      idx_path[320];
    time_t    idx_mtime;
} lib_source_t;

/* (Re)load every mounted card's index. False when MPD couldn't be asked
 * (nothing loaded; `libs_loaded()` stays false). */
bool libs_load(void);
bool libs_loaded(void);

/* True when an index changed on disk, a watched card got one, or (with
 * nothing loaded) a card appeared in MPD since the last load: time to
 * libs_load() again. One cheap stat per source; one short lsinfo only
 * while nothing is loaded. */
bool libs_changed(void);

/* Load when nothing is loaded yet or something changed; true when a load
 * happened (callers rebuild their lists). */
bool libs_refresh(void);

int                  libs_count(void);
const lib_source_t * libs_get(int i);

/* The source and folder record for a folder URI ("card-xxx/Music/A"), or
 * NULL. */
const lib_source_t * libs_find_folder(const char * uri, const lib_folder_t ** folder);

/* Bumped on every load: views compare it to know their pointers are stale. */
unsigned libs_generation(void);

#endif
