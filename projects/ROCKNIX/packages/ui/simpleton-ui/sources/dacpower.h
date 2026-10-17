/*
 * SimpletonOS UI - waking and sleeping the USB DAC with playback (0.21c,
 * 16 Oct 2026; Ian's DX5 II tests the same evening).
 *
 * What the DX5 II showed: asleep (its volume button held, or its own idle
 * timer) it stays on USB and keeps taking audio, so nothing tells the unit
 * and playing to it doesn't wake it - but plugging the cable in does, and
 * so does a USB suspend followed by a resume of its link (the kernel's
 * runtime power management: power/control "auto" lets it suspend, "on"
 * resumes it). A suspended link also puts it to sleep by itself ("Unlock"
 * on its display, its idle sleep a minute later).
 *
 *   Wake    before a Play that follows more than DACPOWER_WAKE_AFTER_S of
 *           silence (or the first one after a start), the DAC's link is
 *           suspended, held down 0.75 s and resumed (under a second, the UI
 *           waits; 0.21c's instant resume didn't wake it - 0.21d) - the "plug it in
 *           again" without unplugging. Setting dac_wake (default on).
 *   Sleep   paused or stopped for dac_sleep seconds (default 300, 0 =
 *           never) the link is allowed to suspend; the next Play resumes it.
 *   Probe   while playing, the DAC's "... Clock Validity" control (if it
 *           has one) is read every two seconds and logged when it changes:
 *           the field test of whether it says "asleep" (the next step pauses
 *           on it).
 *
 * The DAC is the first USB audio card (as simpleton-dac-detect picks it),
 * found again each time, so a swapped DAC is followed. A DAC that doesn't
 * follow USB suspend is left as it was (the cycle costs half a second).
 */
#ifndef SIMPLETON_DACPOWER_H
#define SIMPLETON_DACPOWER_H

#define DACPOWER_WAKE_AFTER_S 60

void dacpower_init(void);
/* MPD's player changed (main.c's notice): playing or not. */
void dacpower_mpd_changed(void);
/* Called from the main loop; acts at most once a second. */
void dacpower_poll(void);
/* mpdc.c calls this before any command that starts playback. */
void dacpower_before_play(void);

#endif
