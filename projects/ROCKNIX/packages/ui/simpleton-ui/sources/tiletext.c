/*
 * SimpletonOS UI - the no-art tile's text (0.17, 10 Oct 2026). Moved out
 * of grid.c unchanged so the album page can lay the same tile in its cover
 * square (the "XXL tile", album-page-6b.md). See tiletext.h.
 */
#include "tiletext.h"
#include "placeholder.h"
#include "textfit.h"
#include "theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIG_TILE_PX   260                     /* no-art tiles this big show the artist too */

/* The no-art text (0.16b, Ian 8 Oct): centred; whole words only, shrinking
 * the type down a ladder of sizes rather than breaking a word across lines;
 * a bracketed tail "(Deluxe Edition)" / "[Disc 2]" set smaller underneath;
 * a little variety in the typeface, steady per album. Only when even the
 * smallest step of the ladder will not take it does it wrap and trail off
 * with dots. */

/* A line of the no-art text, inside the tile's centring box (a flex column
 * that fills the tile, whatever its size: the text stays centred when the
 * tile grows under the cursor). `top` is the space above the line. */
static lv_obj_t * tile_label(lv_obj_t * box, const fit_t * ft, int w, int top)
{
    lv_obj_t * l = lv_label_create(box);
    lv_obj_set_style_text_font(l, ft->font, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(l, ft->space, 0);
    lv_label_set_long_mode(l, ft->whole ? LV_LABEL_LONG_MODE_CLIP : LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(l, w, ft->h);
    lv_obj_set_style_margin_top(l, top, 0);
    lv_label_set_text(l, ft->text);
    return l;
}


void tiletext_make(lv_obj_t * parent, int side, const char * title_in, const char * artist_in, const tile_style_t * st, tiletext_t * out)
{
    memset(out, 0, sizeof(*out));
    int pad = side / 10;
    int w = side - 2 * pad, avail = side - 2 * pad;
    bool big = side >= BIG_TILE_PX;

    /* the theme's trio, used when the theme lists no cover fonts (and as
     * the last fallback): steady per album, a little variety across the
     * screen */
    unsigned hsh = 2166136261u;
    for(const char * p = title_in; *p; p++) hsh = (hsh ^ (unsigned char)*p) * 16777619u;
    bool caps_a = false;
    const char * file_a = placeholder_line_font(1, &caps_a);
    bool trio_caps = false;
    const char * trio_file;
    switch((hsh >> 8) % 3) {
        case 0:  trio_file = placeholder_line_font(2, &trio_caps); break;                   /* the tape's album line */
        case 1:  trio_file = placeholder_line_font(3, &trio_caps); break;                   /* mixed case            */
        default: trio_file = "Caprasimo-Regular.ttf"; trio_caps = false; break;            /* a display face        */
    }

    char title[300], main[300], br[300], artist[300], raw_main[300], raw_br[300];
    snprintf(title, sizeof(title), "%s", title_in);
    snprintf(artist, sizeof(artist), "%s", artist_in ? artist_in : "");
    bool has_br = split_bracket(title, raw_main, sizeof(raw_main), raw_br, sizeof(raw_br));
    if(!has_br) { snprintf(raw_main, sizeof(raw_main), "%s", title); raw_br[0] = 0; }
    if(caps_a) caps_inplace(artist);

    int gap = pad / 2;
    /* artist: up to two lines on its own, shrinking by whole words */
    fit_t fa = { 0 };
    int used_h = 0;
    if(big && artist[0]) {
        int lad[LADDER_STEPS];
        /* side/12, 12..34 px on grid tiles; past 476 px (the page's cover
         * square) it keeps growing at side/14 (Ian, 10 Oct: proportionally
         * larger on the TV square) */
        int start = side / 12 < 12 ? 12 : side / 12 > 34 ? 34 : side / 12;
        if(side / 14 > 34) start = side / 14;
        ladder_fill(lad, start);
        if(fit_rung(file_a, 100, lad, 0, 4, artist, w, avail / 3, 2, &fa) < 0) {
            fa.font = face_px(file_a, lad[4]); fa.px = lad[4]; fa.whole = false; fa.space = 0;
            snprintf(fa.text, sizeof(fa.text), "%s", artist);
            fa.lines = 1; fa.h = lv_font_get_line_height(fa.font);
        }
        used_h = fa.h + gap;
    }

    /* the title: the dealt cover font first, then the next faces that fit
     * its rules, and the theme's trio last (0.16i, Ian's type notes) */
    int room = avail - used_h;
    int max_lines = big ? 5 : 4;
    fit_t fm = { 0 }, fb = { 0 };
    bool placed = false, done = false, show_br = false;
    int lad[LADDER_STEPS];
    int nfaces = tilestyle_font_count();
    int dealt = st ? st->font : TILE_FONT_NONE;
    uint32_t bg = 0, ink = 0;
    if(st) { bg = st->bg; ink = st->ink; }
    int letters = count_letters(raw_main);
    const char * file_t = trio_file;
    bool caps_t = trio_caps;
    int leading = 100;
    for(int k = 0; k < nfaces && !done && dealt != TILE_FONT_NONE; k++) {
        int i = (dealt + k) % nfaces;
        const tile_font_t * f = tilestyle_font(i);
        if(!f || !tilestyle_font_fits(i, letters, bg, ink)) continue;
        snprintf(main, sizeof(main), "%s", raw_main);
        snprintf(br, sizeof(br), "%s", raw_br);
        if(f->caps) { caps_inplace(main); caps_inplace(br); }
        int km = fit_main(f->file, f->leading, f->big, side, w, room, max_lines, main, text_min_px(side) * 3 / 4, lad, &fm);
        placed = km >= 0;
        if(!placed) continue;                                 /* not whole within 3/4 of the floor: a narrower face may take it */
        if(f->min_size > 0 && fm.px < (int)(f->min_size * side)) continue;               /* must be large */
        if(has_br) show_br = fit_tail(f->file, f->leading, side, w, room - fm.h, gap, br, lad, km, &fb);
        file_t = f->file; caps_t = f->caps; leading = f->leading;
        done = true;
    }
    if(!done) {
        /* no face could take it near the floor: the dealt face (else the
         * trio's) goes as small as it must for whole words */
        const tile_font_t * f = dealt != TILE_FONT_NONE ? tilestyle_font(dealt) : NULL;
        const char * file = f ? f->file : trio_file;
        bool caps = f ? f->caps : trio_caps;
        int lead = f ? f->leading : 100;
        snprintf(main, sizeof(main), "%s", raw_main);
        snprintf(br, sizeof(br), "%s", raw_br);
        if(caps) { caps_inplace(main); caps_inplace(br); }
        int km = fit_main(file, lead, f ? f->big : false, side, w, room, max_lines, main, TEXT_MIN_PX, lad, &fm);
        placed = km >= 0;
        if(placed && has_br) show_br = fit_tail(file, lead, side, w, room - fm.h, gap, br, lad, km, &fb);
        file_t = file; caps_t = caps; leading = lead;
    }
    if(!placed) {
        /* the main part alone, trimmed: the tail is the first thing to go */
        fit_fallback(file_t, leading, side, room, max_lines, raw_main, caps_t, &fm);
        show_br = false;
    }
    has_br = show_br;
    static int debug_tiles = -1;
    if(debug_tiles < 0) debug_tiles = getenv("SIMPLETON_DEBUG_TILES") != NULL;
    if(debug_tiles) fprintf(stderr, "simpleton-ui: tile \"%s\": %s %d px, %d line%s, dealt %d%s\n", title_in, file_t, fm.px,
                            fm.lines, fm.lines == 1 ? "" : "s", dealt, done ? "" : " (fallback)");

    lv_obj_t * box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(box, side, side);
    lv_obj_set_style_transform_pivot_x(box, side / 2, 0);
    lv_obj_set_style_transform_pivot_y(box, side / 2, 0);
    out->box = box;
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if(used_h) out->artist = tile_label(box, &fa, w, 0);
    out->title = tile_label(box, &fm, w, used_h ? gap : 0);
    if(has_br) out->extra = tile_label(box, &fb, w, gap / 2);
    out->px = fm.px;
    out->lines = fm.lines;
    out->file = file_t;
    out->fallback = !done;
}

