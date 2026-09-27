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
 *   3. nothing - the screen draws its placeholder.
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
#include <stdint.h>

typedef struct {
    uint8_t * pixels;     /* XRGB8888, stride = w * 4; caller frees   */
    int       w, h;       /* fitted inside the requested box          */
    int       src_w, src_h;
    char      source[64]; /* "embedded", "folder.jpg", ... for logs   */
} art_result_t;

/* Start the worker. `box` is the square the art must fit in (720 here). */
bool art_init(int box);

/* Ask for the art of a track (MPD URI). The latest request wins; an older
 * one still being decoded is discarded when it finishes. */
void art_request(const char * track_uri);

/* UI thread: returns true once with the result of the latest request.
 * `found` false means every source came up empty (draw the placeholder).
 * When found, the caller owns res->pixels. */
bool art_poll(bool * found, art_result_t * res);

#endif
