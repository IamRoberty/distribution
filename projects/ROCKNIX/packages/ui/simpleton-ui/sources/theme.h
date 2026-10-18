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

/* The shared screens' colours (0.25, Ian 17 Oct: "rotating colour themes",
 * three colours at a time, as the track lists do). A visit - entering
 * Settings, Home, the file browser from somewhere else - deals three
 * colours from one of the theme's tilepalette sets: a ground, an ink that
 * reads on it, and a selected colour from the same set that reads on the
 * ground and sits apart from the ink. Everything inside the visit (its
 * sub-pages, pickers, the keyboard, menus, toasts) keeps those three until
 * the next visit; a page with colours of its own (a list page, the album
 * page) sets them, so a menu or toast over it matches. Never the same set
 * twice running. The selected thing grows and takes the selected colour;
 * nothing wears a bar or a bubble. */
typedef struct { uint32_t ground, ink, sel; } ui_colours_t;
void     ui_colours_visit(const char * scope);                     /* a new deal when `scope` differs from the last visit's */
void     ui_colours_redeal(void);                                  /* the next visit deals afresh whatever its scope */
void     ui_colours_set(uint32_t ground, uint32_t ink, uint32_t sel);
const ui_colours_t * ui_colours(void);
/* A pop-up's colours (0.25a, Ian: "the inverse of the color combination
 * behind it, that way things stand out"): the screen's ink as the ground,
 * its ground as the ink, its selected colour when that reads on the new
 * ground, else the cream or the dark ink. */
ui_colours_t ui_colours_inverse(void);
uint32_t ui_ground(void);
uint32_t ui_ink(void);
uint32_t ui_dim(void);          /* the ink thinned toward the ground: hints, group names, values */
uint32_t ui_faint(void);        /* the ink barely there: the keyboard's key lines */
uint32_t ui_accent(void);       /* the selected colour */
uint32_t ui_accent_ink(void);   /* what reads on it: the ground */
uint32_t ui_mix(uint32_t a, uint32_t b, int pct_b);
#define UI_GROW_PCT        108

/* The sign (0.25, Ian: the folder tile's inset keyline, "square signs with
 * a thin border line"): a square-cornered box with a thin line in the ink
 * set in from its edge - the same geometry as the folder tile's frame
 * (inset 5 %, line 1/70 of the shorter side, 2 px at least). ui_sign()
 * makes a box's keyline (a child the box's size, ignored by layout; call
 * ui_sign_fit() after resizing the box); ui_rule() a thin horizontal line
 * in the ink under a header or between groups. */
lv_obj_t * ui_sign(lv_obj_t * box);
void       ui_sign_fit(lv_obj_t * keyline);
void       ui_sign_colour(lv_obj_t * keyline, uint32_t colour);
lv_obj_t * ui_rule(lv_obj_t * parent, int x, int y, int w);
void       ui_rule_colour(lv_obj_t * rule);
int        ui_sign_inset(int side);
/* A list screen's chrome inside the sign: the header (right of the battery
 * corner, under the keyline), the rule under it, and where the list then
 * starts and how tall it is (to the footer). */
typedef struct { int header_x, header_y, header_w, header_h, rule_y, list_y, list_h, footer_y; } ui_sign_chrome_t;
void       ui_sign_chrome(const ui_list_t * lm, ui_sign_chrome_t * out);
int        ui_sign_line(int side);
/* A small header in the theme's bold face, caps (Jost Bold caps - the
 * Archer Ave. look): the text upper-cased into `out`. */
const lv_font_t * ui_font_caps(int px);
void       ui_caps(const char * text, char * out, size_t len);

#define UI_FONT_LIST       ui_font(28)
#define UI_FONT_HINT       ui_font(20)

#endif
