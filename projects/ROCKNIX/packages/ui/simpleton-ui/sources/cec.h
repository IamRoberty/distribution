/*
 * SimpletonOS UI - HDMI-CEC remote input.
 *
 * Lets the TV's own remote drive the UI. Talks to the kernel CEC framework
 * through /dev/cec0 directly (linux/cec.h ioctls): no libcec, no cec-ctl, so
 * no new packages in the image. The kernel's dw-hdmi CEC driver is already
 * live on the RK3566 (confirmed 30 Sep 2026: /dev/cec0 present).
 *
 * Scope (settled: CEC is a nice-to-have, "simple, just works"):
 *   - we appear on the TV as a Playback device named "Simpleton"
 *   - remote keys become the same ui_action_t values the joypad produces
 *   - we answer the handful of polls TVs use to decide whether to forward
 *     remote keys to us (active source, menu status, power status)
 *   - we never turn the TV on and never put it in standby, and the TV going
 *     to standby never stops playback: HDMI is display + remote only
 *
 * Optional everywhere: if /dev/cec0 is missing or the TV has CEC off, every
 * call here is a quiet no-op and the UI behaves exactly as without it.
 */
#ifndef SIMPLETON_CEC_H
#define SIMPLETON_CEC_H

#include <stdbool.h>

/* Open /dev/cec0 and claim a Playback logical address (in the background;
 * the TV may still be off). Returns false if there is no usable adapter. */
bool cec_init(void);

/* File descriptor for poll() (POLLIN | POLLPRI), or -1. */
int cec_fd(void);

/* Call once per main-loop iteration. Never blocks. Answers TV queries and
 * pushes remote keys into the input queue (input_push). */
void cec_poll(void);

#endif
