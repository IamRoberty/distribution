/*
 * SimpletonOS UI - the album page (0.17, 10 Oct 2026; Engineering Note 05
 * section 5 as amended 7 and 10 Oct, build step 6b; album-page-6b.md).
 *
 * What opens when the grid chooses an album: the back of the record.
 *
 *   TV (16:9)   the cover in a stage-sized square on the left, right or
 *               centre; the track list in the rest, or split across both
 *               sides of a centred cover like the sides of a record, with
 *               "Side A / Side B" or "Side 1 / Side 2" above. No names: they
 *               are on the cover. An album without art gets one of three in
 *               the square each visit (0.17h): the grid's no-art tile at
 *               that size (tiletext.h), the cassette, or a record (when a
 *               "record" design is in share/placeholder), the pictures in
 *               the tile's colours with the names on their label and the
 *               playing track's title under them; now-playing's cassette
 *               for a track of the album takes the same colours.
 *   Handheld    no art: artist and album in cover fonts as a heading above
 *               the list, one of them big (the layout's choice); the list
 *               under it, selected from the first track. One page, nothing
 *               changes size (Ian, 10 Oct: a separate names page felt
 *               wrong). Playing a track goes to now-playing, as the
 *               browser does.
 *   The list    titles wrap to three lines, never trimmed to one; numbers
 *               are the layout's choice and hang in a gutter before the
 *               titles, so headers and side labels line up with the titles.
 *               The playing track is in the page's playing colour with a
 *               small triangle a fixed distance before it; the selected one
 *               in the selected colour, grown 8% in place (the lines are
 *               spaced for the grown size, so nothing moves). A favourite
 *               (step 9; dormant until then) shows a red heart after its
 *               title. The list scrolls to keep the selection in view; the
 *               cover stays put.
 *   Colours     from the cover's own palette (library.idx, palette.h): a
 *               background, an ink, a playing colour (the sleeve's lettering
 *               colour where there is one) and a selected colour; a tile
 *               colourway from the theme when the album has no art or no
 *               palette yet.
 *   Layouts     pagelayout lines in the theme (tilestyle.h), one drawn per
 *               visit for the screen.
 *   Moving      Up/Down walk the tracks in order, crossing from the bottom
 *               of the left side to the top of the right; Left/Right jump to
 *               the same row on the other side. Select plays the album from
 *               the track (the whole album queued, as the browser does) and
 *               stays on the page (TV; the handheld goes to now-playing).
 *               Back: the grid, or the browser the album folder was opened
 *               in.
 */
#ifndef SIMPLETON_ALBUMPAGE_H
#define SIMPLETON_ALBUMPAGE_H

#include "lvgl.h"
#include "input.h"
#include "library.h"
#include "tilestyle.h"
#include <stdbool.h>

/* `on_exit`: Back out of the page. `on_play`: a track was started from the
 * page on the handheld (main.c shows now-playing, as after the browser);
 * on a TV the page hosts now-playing in its cover square instead. */
void albumpage_create(void (*on_exit)(void), void (*on_play)(void));

/* Take a copy of the album (title, artist, tracks, palette, thumbnail
 * path) so the library may be reloaded underneath. `card` is the mount's
 * cache name (for the thumbnail); `tile` the style the grid dealt its
 * no-art tile (NULL with art): the page carries its colours and face over.
 * An album whose index line has no palette yet gets one from its 192 px
 * thumbnail here. False when the album has no tracks. */
bool albumpage_open(const library_t * lib, int album, const char * card, const tile_style_t * tile);
/* The same page for a folder the browser is entering (11 Oct): the look of
 * `album` (the one most of its tracks are of), the list every track under
 * `folder` in file order, nothing sorted or folded. */
bool albumpage_open_folder(const library_t * lib, const char * folder, int album, const char * card, const tile_style_t * tile);
/* True when the page was opened as a folder (the restart line says so). */
bool albumpage_by_folder(void);

/* `fresh`: a new visit (the layout and colours are dealt again, the first
 * track selected); false coming back from now-playing or Settings, which
 * keeps the page as it was. */
void albumpage_show(bool fresh);
void albumpage_hide(void);

void albumpage_handle_action(ui_action_t a);

/* MPD's player changed: the playing mark follows the current song. */
void albumpage_mpd_changed(void);
void albumpage_curation_changed(void);       /* a favourite came or went (0.21): the hearts follow */

/* The open album's folder (for the restart line), or NULL. */
const char * albumpage_folder(void);

#endif
