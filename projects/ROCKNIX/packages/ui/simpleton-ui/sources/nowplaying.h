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
 * page's colour), opaque only while the stage shows its art. `stale_ok`:
 * the art up now belongs to an earlier track of the same album, so it may
 * stay up while the new track's is decoded; otherwise art that isn't the
 * expected track's is kept hidden, so the host's own picture shows instead
 * of a flash of another album's cover (Ian, 10 Oct 19:51). */
void nowplaying_embed_art(bool with_art, uint32_t bg, bool stale_ok);
/* The host knows a track just started (before MPD's notice reaches us):
 * art on screen for any other file counts as stale from now. */
void nowplaying_expect_file(const char * uri);
/* The file the art on screen belongs to ("" when none). */
const char * nowplaying_art_file(void);
void nowplaying_unembed(void);
/* Hosted: the album's own cover before any of it plays - the first track's
 * art set (11 Oct); and the "n / m" badge flashed on demand. */
void nowplaying_preview(const char * uri, const char * artist, const char * album, const char * title);
void nowplaying_show_badge(void);
/* Pop the controls up (a track started, or the user asked) and let them
 * fade as usual; hide them now; whether they are up. */
void nowplaying_poke(void);
void nowplaying_hide_controls(void);
bool nowplaying_controls_visible(void);
bool nowplaying_embedded(void);
/* Hosted, showing its own art (decoded and up): the host may hide its own
 * picture under it. */
bool nowplaying_art_ready(void);
/* The page's Select on the cover: the next stop across a wide image, else
 * the next image, round and round; the "n / m" badge shows for a moment
 * even for a single image, so the viewer knows the cover is selected. */
void nowplaying_browse(void);

#endif
