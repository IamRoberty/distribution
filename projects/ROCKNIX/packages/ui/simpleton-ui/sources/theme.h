/*
 * SimpletonOS UI - shared style tokens.
 *
 * One token sheet for every screen (settled 26 Sep 2026). Colors are hex,
 * sizes are derived from the panel's shorter edge so a 16:9 HDMI profile can
 * override UI_BASE alone. Red is deliberately absent: it is a status color
 * (favorites), hardcoded where used, never a theme token.
 */
#ifndef SIMPLETON_THEME_H
#define SIMPLETON_THEME_H

#define UI_COLOR_BG        0x101418   /* near-black background            */
#define UI_COLOR_FG        0xE8E8E8   /* primary text                     */
#define UI_COLOR_DIM       0x8A9099   /* secondary text, hints, header    */
#define UI_COLOR_FOCUS_BG  0x2F6FE0   /* selected row background          */
#define UI_COLOR_FOCUS_FG  0xFFFFFF   /* selected row text                */

/* Layout, in pixels of the 720x720 panel. Row height = 1/9 of the screen so
 * eight rows fit under the header with the hint bar; that is a deliberate
 * "big enough to read from the couch" size, the opposite of the stock app. */
#define UI_BASE            720
#define UI_MARGIN          24
#define UI_ROW_H           80
#define UI_ROW_RADIUS      10
#define UI_HEADER_H        72
#define UI_FOOTER_H        44

#define UI_FONT_LIST       (&lv_font_montserrat_28)
#define UI_FONT_HEADER     (&lv_font_montserrat_20)
#define UI_FONT_HINT       (&lv_font_montserrat_20)

#endif
