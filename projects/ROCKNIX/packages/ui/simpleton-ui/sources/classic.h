/*
 * SimpletonOS UI - the classic visualizers (0.32, 19 Oct 2026;
 * visualizers-research.md). The ones every hi-fi and every Winamp had,
 * drawn by LVGL on the CPU from viz.c's analysis (the feed and the FFT
 * thread; no GPU, so they run on the handheld's panel as they are), each in
 * one of the looks Ian picked from the mock-up sheets (19 Oct 22:12):
 *
 *   spectrum     48 bars on the 12-column grid (four a column), 30 Hz to
 *                18 kHz by log, peak caps that hang and fall
 *   meters       two VU meters with the standard's ballistics (0.33)
 *   scope        the waveforms, and left against right (0.33)
 *   spectrogram  the spectrum over time, loudness as colour (0.33)
 *
 * The looks (a colourway each; the shape is the same):
 *   archer      a colourway dealt from the theme's tile palettes, the sign's
 *               keyline round it (a fresh deal each start)
 *   record      the playing album's own colours (saver_playing_colours);
 *               an album without any gets the archer deal
 *   amp95       black, green-yellow-red bars in LED segments ("Amp 95")
 *   phosphor    a lab scope's green on near-black
 *   amp2000     cyan-to-pink LED segments on midnight, scanlines ("Amp 2000")
 *   ice         cyan on midnight, scanlines
 *   uv          violet with pink caps on deep purple, scanlines
 *   blacklight  magenta on electric blue, cyan caps, scanlines
 *   steam       copper bars, brass rule and caps on soot
 *
 * saver.c runs one as a kind of screensaver (SAVER_SPECTRUM), inside its
 * own root, so the TV rules, the Visualization page and the any-button-ends
 * -it rule all apply unchanged.
 */
#ifndef SIMPLETON_CLASSIC_H
#define SIMPLETON_CLASSIC_H

#include "lvgl.h"
#include <stdbool.h>

typedef enum { CLASSIC_NONE = 0, CLASSIC_SPECTRUM, CLASSIC_METERS, CLASSIC_SCOPE, CLASSIC_SPECTROGRAM } classic_kind_t;

void classic_create(lv_obj_t * parent);                    /* once; `parent` is the saver's root */
bool classic_start(classic_kind_t kind, const char * look); /* the feed on, the drawing timer up */
void classic_stop(void);
void classic_set_look(const char * look);
void classic_keep_colours(void);                           /* the next start keeps a dealt look's colours (0.34: back from Now Playing) */                   /* while it runs: the picture changes at once (vizmenu, 0.32c) */
bool classic_active(void);
classic_kind_t classic_kind_from_code(const char * code);     /* "spectrum" / "meters" / "scope" / "spectrogram" */

/* The spectrogram's direction (0.33a): "rtl" (in at the right), "ltr",
 * "ttb" (in at the top), "btt"; the setting viz-spectrogram-dir. */
int          classic_spg_dir_count(void);
const char * classic_spg_dir_code(int i);
int          classic_spg_dir_name_id(int i);
void         classic_set_spg_dir(const char * code);   /* while it runs: a fresh picture in the new direction */

/* The looks, for Settings: codes and their names' string ids. */
int          classic_look_count(void);
const char * classic_look_code(int i);
int          classic_look_name_id(int i);

#endif
