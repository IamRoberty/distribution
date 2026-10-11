/*
 * SimpletonOS UI - fitting text into a box. See textfit.c.
 *
 * Sizes come from one fixed scale (type_scale) so every screen, face and
 * size shares the same few font objects. A "ladder" is LADDER_STEPS rungs
 * of that scale starting at a given size and going down. The theme's floor
 * (tiletextmin, a fraction of the box side) is the smallest a title may be
 * set at; a single word too wide for a line may dip to min_px below it, a
 * word is never broken; a tail in brackets may go as small as the ladder
 * allows; below all that the text is trimmed with dots.
 */
#ifndef SIMPLETON_TEXTFIT_H
#define SIMPLETON_TEXTFIT_H

#include "lvgl.h"
#include <stdbool.h>
#include <stddef.h>

#define LADDER_STEPS 12
#define TEXT_MIN_PX  16                       /* never below this, whatever the theme says */

typedef struct {
    const lv_font_t * font;
    int  px, lines, h;
    int  space;                 /* line space (negative = tighter than the face's own) */
    bool whole;                 /* every word on one line, nothing trimmed */
    char text[400];
} fit_t;

extern const int type_scale[];

int               text_w(const lv_font_t * f, const char * s);
const lv_font_t * face_px(const char * file, int px);     /* file NULL = the UI face */
/* Greedy word wrap into `out` (lines joined by newlines); *whole is false
 * when a single word is wider than maxw. Returns the line count. */
int  wrap_words(const lv_font_t * f, const char * s, int maxw, char * out, size_t outlen, bool * whole);
void ladder_fill(int * lad, int start);
int  leading_space(const lv_font_t * f, int pct);        /* extra line space for leading pct of the face's own */
int  lines_height(const lv_font_t * f, int lines, int space);
/* The largest rung from `from` to `last` where `text` sits in maxw x maxh
 * in whole words and at most max_lines lines. Returns the rung, or -1. */
int  fit_rung(const char * file, int leading, const int * lad, int from, int last, const char * text,
              int maxw, int maxh, int max_lines, fit_t * out);
bool split_bracket(const char * title, char * main, size_t mlen, char * br, size_t blen);
void caps_inplace(char * s);
int  count_letters(const char * s);
int  text_min_px(int side);                              /* the theme's floor for a box of this side */
int  ladder_floor(const int * lad, int min_px);
/* The main part of a title: biggest rung down to the floor, then below it
 * (to min_px) only for one too-wide word. Returns the rung, or -1. */
int  fit_main(const char * file, int leading, bool big, int side, int w, int room, int max_lines,
              const char * main, int min_px, int * lad, fit_t * fm);
/* The bracketed tail under a placed main part. Returns whether to show it. */
bool fit_tail(const char * file, int leading, int side, int w, int room, int gap, const char * br,
              const int * lad, int k_main, fit_t * fb);
/* Nothing fits whole: wrapped by LVGL at the best size, trailing off with dots. */
void fit_fallback(const char * file, int leading, int side, int room, int max_lines, const char * text, bool caps, fit_t * fm);

#endif
