/*
 * SimpletonOS UI - shared style tokens.
 *
 * One token sheet for the shared screens (settled 26 Sep 2026; amended
 * 4 Oct 2026, Note 05: theme-owned screens - Home, browse views, album page -
 * take layout, art, fonts and colours from the theme instead). Colors are
 * hex. Red is deliberately absent: it is a status color (favorites),
 * hardcoded where used, never a theme token.
 *
 * Sizes (0.11): nothing here is a pixel. layout.h knows the real screen;
 * lists and grids get their measurements from ui_list_metrics() /
 * ui_grid_metrics() (a row count and the screen size), and the shared
 * screens on the square stage measure in stage units through PX().
 */
#ifndef SIMPLETON_THEME_H
#define SIMPLETON_THEME_H

#include "lvgl.h"
#include "layout.h"
#include "config.h"

#define UI_COLOR_BG        0x101418   /* near-black background            */
#define UI_COLOR_FG        0xE8E8E8   /* primary text                     */
#define UI_COLOR_DIM       0x8A9099   /* secondary text, hints, header    */
#define UI_COLOR_FOCUS_BG  0x2F6FE0   /* selected row background          */
#define UI_COLOR_FOCUS_FG  0xFFFFFF   /* selected row text                */

/* The font chain (theme typeface, then Noto for every script - fonts.h) at
 * a size given in stage units, or in real pixels when the size comes from
 * layout metrics. */
const lv_font_t * ui_font(int stage_units);
const lv_font_t * ui_font_px(int px);

/* A font with a particular typeface at its head (a theme's display face for
 * a tape spine, say). `file` is a name in <share>/fonts or an absolute
 * path. With `with_fallbacks` the Noto chain sits behind it as usual;
 * without, it is the typeface alone (cheaper: one FreeType font), for text
 * ui_font_covers() has already passed. */
const lv_font_t * ui_font_face_px(const char * file, int px, bool with_fallbacks);

/* Every letter of `text` (spaces aside) has a real glyph in `font` itself,
 * ignoring its fallbacks. */
bool ui_font_covers(const lv_font_t * font, const char * text);

/* Create a transparent, clipping square of the stage size, centred on
 * `screen`. Shared screens build their layout inside one of these. */
lv_obj_t * ui_stage_create(lv_obj_t * screen);

/* The active theme's name, file and asset folder: config.h (no LVGL there,
 * so the cache tool can read the theme too). */

/* Stage tokens for the shared screens. */
#define UI_BASE            (ui_screen.stage)
#define UI_MARGIN          PX(24)

#define UI_FONT_LIST       ui_font(28)
#define UI_FONT_HINT       ui_font(20)

#endif
