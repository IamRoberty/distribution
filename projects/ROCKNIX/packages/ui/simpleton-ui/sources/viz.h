/*
 * SimpletonOS UI - the visualizers (0.31, 18 Oct 2026;
 * visualizers-research.md). Pictures made from the music itself.
 *
 * The sound comes from MPD's second output ("viz", a fifo carrying the
 * stream exactly as decoded - DSD bits included; a copy, the DAC's stream
 * untouched - proven in 0.30), switched on only while a visualizer is on
 * screen. An audio thread brings it down to 44.1 / 48 kHz itself (0.31g:
 * MPD converting it for us cost 28 % of a core in a realtime thread and
 * starved the DST decoder) and keeps an analysis (levels, a spectrum, where each band sits in the
 * stereo picture, a tempo). A GPU thread draws each frame on the Mali
 * through EGL / GLES2 into a DRM buffer that the display shows on its own
 * plane above the UI (display_viz_*): nothing is copied, nothing read back.
 * Frames are paced by the music - eight a beat once the tempo has locked,
 * a steady 15 a second before that or when there is no beat.
 *
 * A visualizer takes the whole screen; any button but pause and skip ends
 * it (Ian, 18 Oct). It is started and stopped by saver.c, as a kind of
 * screensaver, so the Visualization page and the TV's with-music rule run
 * it like the others.
 *
 *   soundstage   the visible spectrum laid over the audible one: a
 *                pegboard of bulbs, height = pitch (infrared at the bottom
 *                for the sub-bass, ultraviolet at the top past 15 kHz),
 *                width = where the sound sits between the speakers (every
 *                row, the bass too - Ian, 18 Oct 22:20), big bulbs low and
 *                small ones high (Ian's design, 18 Oct 19:46)
 *
 * The music comes first: both threads run at a low priority and the frame
 * rate stops at 24, after DSD skips crackled and DST tracks skipped with
 * the 0.31e visualizer running (0.31f).
 */
#ifndef SIMPLETON_VIZ_H
#define SIMPLETON_VIZ_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { VIZ_NONE = 0, VIZ_SOUNDSTAGE, VIZ_FEED } viz_kind_t;   /* VIZ_FEED: the feed and analysis only, for a visualizer drawn by LVGL (classic.c, 0.32) */

#define VIZ_BANDS 48        /* the classics' spectrum: 30 Hz - 18 kHz by log, four a grid column */
#define VIZ_SPG_ROWS 240    /* the spectrogram's rows, the same range (0.33) */

/* The display has a plane for it and the GPU libraries load. */
bool viz_available(void);

/* Start one (the feed on, the threads up, the plane shown on the first
 * frame); false when the feed can't be switched on or the GPU won't start. */
bool viz_start(viz_kind_t kind);
void viz_stop(void);
bool viz_active(void);

/* The player changed (main.c, on MPD's player event): the feed's format may
 * have, and the reader follows MPD's status "audio" (0.31g). */
void viz_mpd_changed(void);

/* For the classics (0.32): the spectrum's bands 0..1 (returns how many, 0
 * when nothing runs) and the last moment's RMS levels 0..1. */
int  viz_bands(float * out, int max);
void viz_levels(float * l, float * r);
int  viz_spectrogram(float * out, int max);                 /* the newest column, bottom row first (0.33) */
int  viz_wave(int16_t * l, int16_t * r, int n);             /* the newest n samples (up to 4096), oldest first (0.33) */
void viz_set_format(const char * audio);     /* "dsd64:2" / "96000:24:2"; ignored unless it parses */

#endif
