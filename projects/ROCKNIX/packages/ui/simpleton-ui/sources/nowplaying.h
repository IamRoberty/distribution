/*
 * SimpletonOS UI - now-playing screen.
 *
 * Full-bleed album art on the square stage (the whole 720x720 panel, or a
 * centred square on a TV), with the controls floating on
 * a translucent strip over the bottom edge. Three focusable rows (settled
 * design, ui-design.md): scrub bar, transport, and heart / info / options.
 * The strip fades out after a few seconds; with it hidden, the device is
 * just the album cover. Focus defaults to the transport row on entry.
 *
 * Actions are delivered directly by main.c while this screen is active;
 * nothing here goes through LVGL's keypad group.
 */
#ifndef SIMPLETON_NOWPLAYING_H
#define SIMPLETON_NOWPLAYING_H

#include "lvgl.h"
#include "input.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum { NP_HANDLED, NP_EXIT } np_result_t;

/* Build the screen (not loaded yet). Starts the art worker. */
void nowplaying_create(void);

/* Load the screen: controls shown, transport row focused, status refreshed. */
void nowplaying_show(void);

/* Called by main.c after it has loaded another screen. */
void nowplaying_hide(void);

/* MPD said playback, volume, options or the queue changed: re-read status
 * (now if the screen is showing, else when it is next shown). */
void nowplaying_mpd_changed(void);

/* MPD is away (restarting for a new DAC): the format line says "Switching
 * output..." until the next change notice finds it back (0.18). */
void nowplaying_mpd_away(void);

/* Handle any action while active. NP_EXIT = user backed out to the browser;
 * the caller switches screens. */
np_result_t nowplaying_handle_action(ui_action_t a);

/* ---- hosted inside the album page on a TV (0.17b, Ian 10 Oct) ----
 *
 * The page's cover square is the now-playing stage: the stage object moves
 * into the page's screen at (x, y) and comes alive (status, controls,
 * idle fade) without its own screen being shown. `with_art`: show the
 * playing track's own art in the square (the cover of the album being
 * viewed, decoded full size and pageable) - false keeps the page's own
 * picture underneath and hides the stage's (another album is playing).
 * The page forwards actions with nowplaying_handle_action() while the user
 * is "in" now-playing; NP_EXIT hands them back. */
void nowplaying_embed(lv_obj_t * parent, int x, int y, bool with_art, uint32_t bg);
/* `bg`: the stage's own background behind art that isn't square (the
 * page's colour), opaque only while the stage shows its art. */
void nowplaying_embed_art(bool with_art, uint32_t bg);
void nowplaying_unembed(void);
/* Hosted: the album's own cover before any of it plays - the first track's
 * art set (11 Oct); and the "n / m" badge flashed on demand. */
void nowplaying_preview(const char * uri, const char * artist, const char * album, const char * title);
void nowplaying_show_badge(void);
bool nowplaying_embedded(void);
/* Hosted on a TV album page (0.23e, Ian: "we still want the now-playing
 * overlay to pop up over the art when we play a new track"): the strip in
 * the page's colours; pop it up (it fades as usual) - while it is up the
 * page forwards actions to nowplaying_handle_action(); take it down; is
 * it up. */
/* `bg` is the host picture's own background (Ian, 17 Oct 16:13: "the same
 * colour as the background of the album image, universally"): the strip
 * takes it as it is (a placeholder's), or pushed dark (`darken`, a real
 * cover's palette colour); the ink is chosen to read on it; `sel` is the
 * accent when it reads, else one of the theme's. When the stage shows a
 * cover of its own (with_art), the cover's colours win as on the screen. */
void nowplaying_embed_colours(uint32_t bg, uint32_t sel, bool darken);
void nowplaying_poke(void);
void nowplaying_hide_controls(void);
bool nowplaying_controls_visible(void);
/* Hosted, showing its own art (decoded and up): the host may hide its own
 * picture under it. */
bool nowplaying_art_ready(void);
/* The page's Select on the cover: the next stop across a wide image, else
 * the next image, round and round; the "n / m" badge shows for a moment
 * even for a single image, so the viewer knows the cover is selected. */
void nowplaying_browse(void);
/* 0.24 (Ian, 17 Oct): hold A on the album page's art enters "art browse":
 * the "n / m" badge stays up and Left/Right turn the pages (there, they
 * otherwise flip the page or cross to the list). pages > 1 says there is
 * something to browse (several images, or one wide image with stops). */
int  nowplaying_art_pages(void);
void nowplaying_browse_dir(int dir);
void nowplaying_badge_hold(bool hold);

/* 0.21: the Favorite button toggled the playing track's heart (main.c
 * tells the other screens); a heart changed elsewhere (the button follows). */
void nowplaying_set_curation_callback(void (*on_changed)(void));
void nowplaying_curation_changed(void);

#endif
