/*
 * SimpletonOS UI - a visualizer's menu over it (0.32c, 19 Oct 2026; Ian,
 * 23:22). Hold A on the Visualization page: the visualizer starts and a
 * small sign sits over it with what it has to set - Spectrum's look; for
 * Bounce, in Developer mode, a bare number. Left and Right change the
 * setting and the picture behind follows at once; A or B closes the box
 * and leaves the visualizer running (then any button ends it, as ever).
 * A visualizer with nothing to set gets no box.
 */
#ifndef SIMPLETON_VIZMENU_H
#define SIMPLETON_VIZMENU_H

#include "input.h"
#include "saver.h"
#include <stdbool.h>

void vizmenu_create(void);
bool vizmenu_has_options(saver_kind_t kind);     /* something to show for this kind */
void vizmenu_open(saver_kind_t kind);            /* over the running one */
void vizmenu_close(void);
bool vizmenu_active(void);
void vizmenu_handle_action(ui_action_t a);

#endif
