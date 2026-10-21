/*
 * SimpletonOS UI - syncing the picture with the sound (0.35, 20 Oct 2026;
 * Ian's design, 21:10). The visualizers' feed runs ahead of what the DAC
 * plays (MPD's output buffer, the DAC's own; a TV's picture delay pulls the
 * other way), so the visuals are held back by one delay shared by all of
 * them (viz-delay, 0-1000 ms; viz_set_delay_ms). This page sets it by ear:
 * a big light in the middle flips between black and white on each beat the
 * analysis hears in whatever is playing; Left and Right move the delay 10
 * ms at a time (held: faster) until the flips land on the beats you hear.
 * The change is live - no reset. Back leaves. Opened from Settings →
 * Visualization → Sync with the sound.
 */
#ifndef SIMPLETON_SYNC_H
#define SIMPLETON_SYNC_H

#include "input.h"
#include <stdbool.h>

void sync_create(void);
void sync_open(void);
void sync_close(void);
bool sync_active(void);
void sync_handle_action(ui_action_t a);

#endif
