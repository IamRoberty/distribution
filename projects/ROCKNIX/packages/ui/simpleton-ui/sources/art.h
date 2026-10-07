/*
 * SimpletonOS UI - album art loader.
 *
 * Finds, decodes and scales cover art on a worker thread so the UI never
 * stalls on a 3000x3000 JPEG. Lookup order (settled 27 Sep 2026):
 *
 *   1. picture embedded in the track's tags, via MPD `readpicture`
 *      (for a CUE-sheet track, the real audio file behind the sheet);
 *   2. an image file in the album folder: cover / folder / front / album,
 *      then an Artwork / Scans / Covers subfolder, preferring a file with
 *      "front" or "cover" in its name;
 *   3. designed placeholder art (placeholder.h): the theme's cassette or
 *      record, tinted in a colourway of the theme, with the tags on its
 *      label - so the screen still shows a "cover";
 *   4. nothing usable at all (assets missing) - the screen draws its plain
 *      placeholder.
 *
 * MPD's own `albumart` is deliberately not used: it only recognises
 * cover.jpg/png/webp, which real libraries (folder.jpg, Artwork/) fail.
 *
 * Threading: the worker never touches LVGL. The UI thread calls
 * art_request() when the track changes and art_poll() from a timer; a
 * result is handed over under a mutex and rendered by the caller.
 */
#ifndef SIMPLETON_ART_H
#define SIMPLETON_ART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t * pixels;     /* XRGB8888, stride = w * 4; caller frees   */
    int       w, h;       /* fitted inside the requested box          */
    int       src_w, src_h;
    char      source[64]; /* "embedded", "folder.jpg", ... for logs   */
    int       index;      /* which of the album's images this is      */
    int       count;      /* how many the album has (1 = no paging)   */
} art_result_t;

/* Start the worker. `box` is the square the art must fit in (720 here). */
bool art_init(int box);

/* Ask for the art of a track (MPD URI). The tags are what the designed
 * placeholder writes on its label when no real art turns up. The latest
 * request wins; an older one still being decoded is discarded when it
 * finishes. */
void art_request(const char * track_uri, const char * artist, const char * album, const char * title);

/* Show another of the current album's images (art paging, D-pad left/right
 * with the controls hidden). Page 0 is the cover chosen by the lookup order
 * above; the rest are every other image in the album folder and its
 * Artwork/Scans/... subfolders, alphabetical. Out-of-range wraps. Pages
 * wider than 1.2:1 come back at the full box height (up to 4:1 wide) for
 * the caller to pan across; the cover and tall images are fitted. On a
 * track change within the same album folder the page being viewed is kept;
 * a new album starts on its cover. */
void art_request_page(int index);

/* UI thread: returns true once with the result of the latest request.
 * `found` false means nothing at all could be drawn (plain placeholder).
 * When found, the caller owns res->pixels. */
bool art_poll(bool * found, art_result_t * res);

/* ---- for the cache builder (simpleton-cache), single-threaded, no worker ----
 *
 * The album cover's encoded bytes for a track, by the lookup order above
 * (embedded via MPD, the folder's named image, else the first image in the
 * folder). Caller frees. `source` says where it came from. NULL = none. */
uint8_t * art_fetch_cover(const char * track_uri, size_t * len, char * source, size_t slen);

/* Decode JPEG/PNG bytes and fit the picture inside a `box` square:
 * XRGB8888, stride w * 4, caller frees. NULL when undecodable. */
uint8_t * art_decode_fit(const uint8_t * data, size_t len, int box, int * w, int * h);

#endif
