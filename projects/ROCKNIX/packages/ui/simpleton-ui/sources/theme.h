/*
 * SimpletonOS UI - shared style tokens.
 *
 * One token sheet for every screen (settled 26 Sep 2026). Colors are hex.
 * Red is deliberately absent: it is a status color (favorites), hardcoded
 * where used, never a theme token.
 *
 * Sizes: every layout is drawn on a square "stage" (29 Sep 2026) whose side
 * is the screen's shorter edge - 720 on the panel, 1080 on a 1080p TV - and
 * which sits centred on the screen. All pixel numbers in the code are
 * written for the 720 panel and go through PX(), which scales them to the
 * real stage; fonts go through ui_font(), which picks the nearest built-in
 * size. So the panel looks exactly as it did, and a TV gets the same
 * composition at 1.5x.
 */
#ifndef SIMPLETON_THEME_H
#define SIMPLETON_THEME_H

#include "lvgl.h"

#define UI_COLOR_BG        0x101418   /* near-black background            */
#define UI_COLOR_FG        0xE8E8E8   /* primary text                     */
#define UI_COLOR_DIM       0x8A9099   /* secondary text, hints, header    */
#define UI_COLOR_FOCUS_BG  0x2F6FE0   /* selected row background          */
#define UI_COLOR_FOCUS_FG  0xFFFFFF   /* selected row text                */

/* Stage side in real pixels; set once by main.c before any screen is built. */
extern int ui_base;

/* A length designed on the 720 panel, scaled to the real stage. */
#define PX(v)              ((int32_t)(((v) * ui_base + 360) / 720))

/* Built-in Montserrat nearest to `design_px` (a size on the 720 panel)
 * after scaling to the stage. */
const lv_font_t * ui_font(int design_px);

/* Create a transparent, clipping square of the stage size, centred on
 * `screen`. Every screen builds its layout inside one of these. */
lv_obj_t * ui_stage_create(lv_obj_t * screen);

/* Layout, designed in pixels of the 720x720 panel. Row height = 1/9 of the
 * screen so eight rows fit under the header with the hint bar; that is a
 * deliberate "big enough to read from the couch" size, the opposite of the
 * stock app. */
#define UI_BASE            ui_base
#define UI_MARGIN          PX(24)
#define UI_ROW_H           PX(80)
#define UI_ROW_RADIUS      PX(10)
#define UI_HEADER_H        PX(72)
#define UI_FOOTER_H        PX(44)

#define UI_FONT_LIST       ui_font(28)
#define UI_FONT_HEADER     ui_font(20)
#define UI_FONT_HINT       ui_font(20)

#endif
