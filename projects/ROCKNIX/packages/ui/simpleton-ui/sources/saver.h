/*
 * SimpletonOS UI - the TV's screensavers (0.29, 18 Oct 2026;
 * stationary-mode-proposal.md). On a TV the picture never dims or goes off
 * by itself: after the idle time power.c starts one of these over
 * everything, and any press ends it. The music is never touched.
 *
 *   off        a black picture, the signal kept (the TV stays on, dark)
 *   drift      Ian's "art drift": covers from the thumbnail cache at three
 *              sizes and squares in the palette's colours appear, sit and
 *              fade over a ground that drifts to another palette every
 *              minute or two; a piece may be overlapped once it is under
 *              half opacity
 *   cover      the playing album's cover, large, drifting slowly over a
 *              ground in its own colours, the title and artist in a corner;
 *              changes with the track
 *   palettes   art drift without the art - squares only
 *   bounce     the playing cover drifting and bouncing off the edges like
 *              the old DVD player's logo (Ian, 18 Oct 16:24); it follows
 *              the track. (What happens in a corner is a surprise.)
 *
 * The TV rules start one by themselves (power.c); the Home shelf's
 * Visualization tape lets anyone start one by hand (saver_start_manual):
 * that one runs on either screen until a press, and power.c leaves it be.
 */
#ifndef SIMPLETON_SAVER_H
#define SIMPLETON_SAVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { SAVER_NONE = 0, SAVER_OFF, SAVER_DRIFT, SAVER_COVER, SAVER_PALETTES, SAVER_BOUNCE, SAVER_STAGE, SAVER_SPECTRUM, SAVER_METERS, SAVER_SCOPE, SAVER_SPECTROGRAM, SAVER_KIND_COUNT } saver_kind_t;

/* The visualizers (0.31, viz.h) run as kinds of screensaver so the
 * Visualization page and the TV's with-music rule start them like the rest;
 * they need music, so the nothing-playing chooser never offers them. */
bool saver_kind_is_viz(saver_kind_t kind);
void saver_mpd_changed(void);                 /* the player changed: a visualizer ends on a DST track (0.31j) */
/* The playing album's colours for a classic's "record" look (classic.c, 0.32). */
void saver_set_bounce_every(int n);          /* the developer's number, while Bounce runs (vizmenu, 0.32c) */
bool saver_playing_colours(uint32_t * ground, uint32_t * bar, uint32_t * cap, char * file, size_t file_len);

void         saver_create(void);
void         saver_start(saver_kind_t kind);
void         saver_start_manual(saver_kind_t kind);   /* from the Visualization page */
bool         saver_manual(void);
void         saver_stop(void);
bool         saver_active(void);
saver_kind_t saver_current(void);

/* The setting codes: "off", "drift", "cover", "palettes", "bounce", "stage", "spectrum", "meters", "scope", "spectrogram". */
saver_kind_t saver_kind_from_code(const char * code);
const char * saver_kind_code(saver_kind_t kind);

#endif
