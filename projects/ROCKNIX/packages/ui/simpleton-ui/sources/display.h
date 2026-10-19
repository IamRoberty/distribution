/*
 * SimpletonOS UI - display output (DRM/KMS), our own backend.
 *
 * Replaces LVGL's lv_linux_drm driver, which always takes the first
 * connected connector at its native mode: with a TV plugged in at boot that
 * meant 3840x2160 on the TV (11x the pixels of the panel, far too many for
 * software rendering) and the panel left showing the text console.
 *
 * Rules (decided 29 Sep 2026):
 *   - one output at a time: HDMI when it's connected, else the panel;
 *     everything else is switched off in the same modeset, so the unused
 *     screen goes dark instead of showing the console;
 *   - on HDMI, never more than 1920x1080 (prefers 1080p60) - the TV scales;
 *   - the layout is a square "stage" sized to the screen's shorter edge and
 *     centred; the space beside it belongs to whichever screen is showing.
 *
 * Hotplug: the kernel's uevents are watched directly (netlink), so plugging
 * or unplugging HDMI is noticed without udev rules or the ROCKNIX hotplug
 * script (that one only acts when EmulationStation is running).
 */
#ifndef SIMPLETON_DISPLAY_H
#define SIMPLETON_DISPLAY_H

#include "lvgl.h"
#include <stdbool.h>

typedef struct {
    int  w, h;             /* output resolution                          */
    int  stage;            /* side of the square stage (shorter edge)    */
    bool external;         /* HDMI / DP, as opposed to the built-in panel */
    char name[32];         /* "HDMI-A-1", "DSI-1" - for the log           */
} display_info_t;

/* Pick the output, modeset it, and create the LVGL display on it. */
lv_display_t * display_init(display_info_t * info);

/* Hotplug socket for poll(), or -1. */
int display_hotplug_fd(void);

/* Call every main-loop pass. Returns true once the connected outputs have
 * settled (debounced) into a state where a different output should be used
 * than the one we're on - the caller then restarts the UI on it. */
bool display_hotplug_poll(void);

/* The visualizer's own plane (0.31, visualizers-research.md): a second
 * DRM plane above the UI's, showing a buffer the GPU draws into - nothing
 * is ever copied. Buffers are dumb buffers from the display's own device,
 * exported as dma-bufs for EGL to import. display_viz_show() may be called
 * from the GPU thread; it commits blocking (no event) under the display's
 * lock, so the UI's own flips stay as they are. The plane scales the
 * buffer to the screen, so a half-size buffer is fine. */
typedef struct {
    int      w, h;
    int      dmabuf_fd;       /* for EGL_LINUX_DMA_BUF_EXT */
    uint32_t pitch, fb, handle;
} display_vizbuf_t;

bool display_viz_alloc(display_vizbuf_t * b, int w, int h);
void display_viz_free(display_vizbuf_t * b);
bool display_viz_show(const display_vizbuf_t * b);     /* on screen, full size (scaled) */
void display_viz_hide(void);
bool display_viz_available(void);                      /* a second plane exists for this output (probed once) */
bool display_viz_scales(void);                         /* ...and it scales a half-size buffer to the screen */

#endif
