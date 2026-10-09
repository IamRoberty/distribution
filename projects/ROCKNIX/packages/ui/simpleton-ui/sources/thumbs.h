/*
 * SimpletonOS UI - thumbnail decoder for the grids (0.16, 7 Oct 2026;
 * Engineering Note 05 section 4, build step 6a).
 *
 * The cache (cache.h) keeps every album's cover at four sizes as small
 * JPEGs. A grid asks for the ones it can see, as XRGB8888 at the exact
 * pixel size of its tiles, and this worker decodes them off the UI thread
 * so the cursor never waits for a picture (Note 05 principle 4).
 *
 * The grid hands over its whole wanted list every time the view moves:
 * the list replaces the previous one, so tiles flown past are dropped
 * before they are decoded, and the order of the list is the order of
 * decoding (what is on screen first, the pages ahead after). One decode
 * is in flight at a time; its result is kept even if it is no longer
 * wanted (the grid's own cache decides).
 */
#ifndef SIMPLETON_THUMBS_H
#define SIMPLETON_THUMBS_H

#include <stdbool.h>
#include <stdint.h>

#define THUMBS_MAX_WANT 256

typedef struct {
    int  id;             /* the caller's handle (an album's index)     */
    int  gen;            /* the caller's list generation (results echo it) */
    int  px;             /* decode fitted inside px x px               */
    char path[320];      /* the cached JPEG                            */
} thumb_req_t;

typedef struct {
    int       id, gen, px;
    uint8_t * pixels;    /* XRGB8888, stride w * 4; caller frees. NULL = not decodable */
    int       w, h;
} thumb_res_t;

bool thumbs_init(void);

/* Replace the wanted list. */
void thumbs_want(const thumb_req_t * reqs, int count);

/* UI thread: one finished decode, or false when none is ready. */
bool thumbs_poll(thumb_res_t * out);

#endif
