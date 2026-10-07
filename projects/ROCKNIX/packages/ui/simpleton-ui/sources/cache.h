/*
 * SimpletonOS - the library cache builder (0.12, 4 Oct 2026; Engineering
 * Note 05 section 9, build step 3c).
 *
 * For one card (an MPD mount, card-<serial>) it builds and keeps:
 *
 *   <system card>/storage/.simpleton/cache/<card>/library.idx    the index (library.h)
 *   .../thumbs/<album key>_<size>.jpg                           cover thumbnails at
 *                                                               lib_thumb_px sizes
 *
 * and mirrors the lot to <music card>/.simpleton/cache/, so the cache
 * travels with the card and a unit that has never seen the card copies it
 * in instead of rebuilding. Every album carries a fingerprint (its tracks'
 * URIs and dates); only albums whose fingerprint changed, or whose
 * thumbnails are missing, are looked up and decoded again. Each file is
 * written under a temporary name and renamed, so a card pulled mid-write
 * keeps what it had, and the index is written last. Writing to the card is
 * a setting (/storage/.config/simpleton/cache_on_card = off stops it), and
 * nothing is written when the card is nearly full.
 *
 * Run by simpleton-cache@<card>.service after each scan, at idle priority:
 * nothing here may touch playback.
 *
 * Not yet (later steps): artist images and mosaics, tape spines, eviction
 * of caches for cards not seen in a long time.
 */
#ifndef SIMPLETON_CACHE_H
#define SIMPLETON_CACHE_H

#include <stdbool.h>
#include <stdint.h>
#include "library.h"

/* Where the system keeps caches; one folder per card below it. */
#define CACHE_ROOT_DEFAULT   "/storage/.simpleton/cache"
#define CACHE_ON_CARD_DIR    ".simpleton/cache"
#define CACHE_SETTING        "/storage/.config/simpleton/cache_on_card"

/* Build or refresh the cache of `card` (an MPD mount name). `verbose` logs
 * every album. Returns false when MPD or the card could not be read; the
 * cache on disk is then left as it was. */
bool cache_build(const char * card, bool verbose);

/* Path of a thumbnail for an album at a size index (0..LIB_THUMB_COUNT-1). */
void cache_thumb_path(const char * card, const lib_album_t * album, int size_index, char * out, size_t len);

/* Path of the index file of a card's cache. */
void cache_index_path(const char * card, char * out, size_t len);

/* Album key: what the thumbnail files are named after (hex, 16 chars). */
void cache_album_key(const lib_album_t * album, char * out, size_t len);

/* The dominant colour of a picture (XRGB8888), 0xRRGGBB. */
uint32_t cache_dominant_colour(const uint8_t * px, int w, int h);

#endif
