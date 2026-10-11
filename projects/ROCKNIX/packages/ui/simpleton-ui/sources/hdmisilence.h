/*
 * SimpletonOS UI - digital silence on the HDMI audio link (0.17j, 11 Oct 2026).
 *
 * Some TVs hum or buzz when an HDMI source sends no audio at all (Ian, 8 and
 * 10 Oct). The stock ROCKNIX image kept the HDMI sound card busy; ours never
 * opened it (the 8 Oct readings: HDMI audio clock off on ours, on in stock).
 * While the UI runs on an HDMI screen this keeps the HDMI sound card open and
 * feeds it zeros - 48 kHz, 16 bit, stereo - on a low-priority thread.
 *
 * Music is not affected: MPD only ever opens a USB-Audio card (its device
 * line is written by simpleton-dac-detect), and this is silence, not a
 * second output. HDMI stays "display and remote control only".
 *
 *   /storage/.config/simpleton/hdmi_silence   off = don't (default on)
 *   SIMPLETON_HDMI_PCM=<alsa device>           testing: use this device
 */
#ifndef SIMPLETON_HDMISILENCE_H
#define SIMPLETON_HDMISILENCE_H

#include <stdbool.h>

/* Start feeding silence to the HDMI sound card, if there is one and the
 * setting allows it. Call once, after the display is up, only when the
 * screen is an external one. Returns false (with a log line) when it won't
 * run; never blocks. */
bool hdmi_silence_start(void);

#endif
