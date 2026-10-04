/*
 * SimpletonOS UI - play through folders.
 *
 * Settled 28 Sep / 1 Oct 2026, behaviour exactly like HiBy: when the last
 * track of a folder finishes, playback continues with the next folder - go
 * up to the parent, enter the next folder after the current one (descending
 * into it until a folder with tracks is found); if there is none, keep
 * climbing and looking; stop only after the last track in the library root.
 * On by default; off with the Settings toggle. Does nothing in repeat or
 * single (repeat-one) mode. Works in random mode too.
 *
 * How: driven by MPD's `idle` notices, never by polling. When the track
 * that just started is the last one in the queue (MPD reports no `nextsong`)
 * the next folder's tracks are appended to the queue right away - so MPD
 * already holds the next song when this one ends and the transition is
 * gapless, exactly as within an album. The queue grows one folder at a time
 * as you listen; `previous` therefore still walks back across albums.
 *
 * "Folder" and "tracks of a folder" mean what the browser shows
 * (mpd_lsinfo_expanded): a SACD ISO, a multi-track DFF or a CUE album is
 * part of the folder holding it, never a folder of its own, and a folder
 * holding two discs plays both.
 *
 * Nothing is kept across a UI restart (HDMI hotplug): the current song's
 * URI in MPD is all this needs. This module talks only to mpdc.h, so it can
 * move unchanged into a small daemon beside MPD when the headless products
 * arrive; then buttons, CEC, the phone app and the headless box all behave
 * identically because the logic runs in one place.
 */
#ifndef SIMPLETON_PLAYTHROUGH_H
#define SIMPLETON_PLAYTHROUGH_H

#include <stdbool.h>

/* Setting file (Settings page writes it; absent = on):
 *   /storage/.config/simpleton/play_through   "1" / "0" */
bool playthrough_enabled(void);

/* Hand over MPD's change bits (MPD_CHANGED_*). Cheap for bits it ignores. */
void playthrough_mpd_changed(unsigned what);

#endif
