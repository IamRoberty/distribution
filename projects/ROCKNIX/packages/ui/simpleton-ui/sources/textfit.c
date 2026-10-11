/*
 * SimpletonOS UI - fitting text into a box (0.17, 10 Oct 2026).
 *
 * The no-art tiles' type machinery from 0.16b-0.16l, moved out of grid.c
 * so the album page (and anything after it) sets its headers and lists the
 * same way: whole words only, shrinking down a fixed scale of sizes rather
 * than breaking a word; a floor on the size (the theme's tiletextmin) with
 * "..." below it; a bracketed tail "(Deluxe Edition)" set smaller; leading
 * as a percentage of a face's own line height. grid.c is unchanged in what
 * it draws (checked pixel for pixel in the tile harness).
 */
#include "textfit.h"
#include "theme.h"
#include "tilestyle.h"
#include "strings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


int text_w(const lv_font_t * f, const char * s)
{
    lv_point_t sz;
    lv_text_get_size(&sz, s, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

const lv_font_t * face_px(const char * file, int px)
{
    return file ? ui_font_face_px(file, px, true) : ui_font_px(px);
}

/* Greedy word wrap into `out` (lines joined by '\n'); *whole is false when
 * a single word is wider than maxw. Returns the line count. */
int wrap_words(const lv_font_t * f, const char * s, int maxw, char * out, size_t outlen, bool * whole)
{
    int lines = 0, linew = 0;
    size_t used = 0;
    int space_w = text_w(f, " ");
    *whole = true;
    out[0] = 0;
    const char * p = s;
    while(*p) {
        while(*p == ' ') p++;
        if(!*p) break;
        char word[160];
        size_t n = 0;
        while(p[n] && p[n] != ' ' && n < sizeof(word) - 1) n++;
        memcpy(word, p, n);
        word[n] = 0;
        p += n;
        int ww = text_w(f, word);
        if(ww > maxw) *whole = false;
        if(lines == 0) { lines = 1; linew = ww; }
        else if(linew + space_w + ww <= maxw) { if(used + 1 < outlen) out[used++] = ' '; linew += space_w + ww; }
        else { if(used + 1 < outlen) out[used++] = '\n'; lines++; linew = ww; }
        size_t wl = strlen(word);
        if(used + wl < outlen) { memcpy(out + used, word, wl); used += wl; }
        out[used] = 0;
    }
    return lines;
}



/* Sizes come from one fixed scale (~12% a step) so every tile size, face
 * and grid size shares the same few font objects: with seventeen faces,
 * free sizes would soon run the font cache out (0.16i). */
const int type_scale[] = { 10, 11, 12, 14, 16, 18, 20, 22, 25, 28, 32, 36, 40, 45, 50, 56, 63, 72, 80, 90, 100, 112, 126, 140, 160, 180 };
#define TYPE_STEPS ((int)(sizeof(type_scale) / sizeof(type_scale[0])))

void ladder_fill(int * lad, int start)
{
    int k = 0;
    while(k + 1 < TYPE_STEPS && type_scale[k + 1] <= start) k++;
    for(int i = 0; i < LADDER_STEPS; i++) lad[i] = type_scale[k - i < 0 ? 0 : k - i];
}

/* Leading (Ian, 9 Oct): a face's lines set at `pct` of its own line
 * height; the extra (usually negative) space between lines. */
int leading_space(const lv_font_t * f, int pct)
{
    int lh = lv_font_get_line_height(f);
    return lh * (pct - 100) / 100;
}

int lines_height(const lv_font_t * f, int lines, int space)
{
    return lines * lv_font_get_line_height(f) + (lines > 1 ? (lines - 1) * space : 0);
}

/* The largest rung from `from` on (to `last`) where `text` sits in maxw x maxh
 * in whole words and at most max_lines lines. Returns the rung, or -1. */
int fit_rung(const char * file, int leading, const int * lad, int from, int last, const char * text,
                    int maxw, int maxh, int max_lines, fit_t * out)
{
    for(int k = from; k <= last; k++) {
        const lv_font_t * f = face_px(file, lad[k]);
        bool whole;
        int lines = wrap_words(f, text, maxw, out->text, sizeof(out->text), &whole);
        int space = leading_space(f, leading);
        int h = lines_height(f, lines, space);
        if(k > from && lad[k] == lad[k - 1]) break;            /* the ladder has bottomed out */
        if(whole && lines <= max_lines && h <= maxh) {
            out->font = f; out->px = lad[k]; out->lines = lines; out->h = h; out->space = space; out->whole = true;
            return k;
        }
    }
    return -1;
}

/* Split "Title (Deluxe Edition)" at its first bracket; returns false when
 * there is nothing before it or nothing in it. */
bool split_bracket(const char * title, char * main, size_t mlen, char * br, size_t blen)
{
    const char * q = NULL;
    for(const char * p = title; *p; p++) if(*p == '(' || *p == '[') { q = p; break; }
    if(!q || q == title) return false;
    size_t n = (size_t)(q - title);
    while(n > 0 && title[n - 1] == ' ') n--;
    if(n == 0 || n >= mlen) return false;
    memcpy(main, title, n);
    main[n] = 0;
    snprintf(br, blen, "%s", q);
    return br[0] != 0;
}

void caps_inplace(char * s)
{
    str_upper_utf8(s);          /* accented, Greek, Cyrillic too (11 Oct) */
}


/* Letters in a title (for the cover-font rules): everything but spaces and
 * punctuation, counted as code points. */
int count_letters(const char * s)
{
    int n = 0;
    for(const unsigned char * p = (const unsigned char *)s; *p; p++) {
        if((*p & 0xC0) == 0x80) continue;                        /* UTF-8 continuation */
        if(*p < 0x80 && !((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))) continue;
        n++;
    }
    return n;
}

/* The smallest size a title may be set at (Ian, 10 Oct: "set a size limit
 * for how small a font can get; use the ... before something gets too
 * small to read"): the theme's fraction of the tile, never under
 * TEXT_MIN_PX. */
int text_min_px(int side)
{
    int m = (int)(tilestyle_text_min() * (float)side);
    return m < TEXT_MIN_PX ? TEXT_MIN_PX : m;
}

/* The last rung of the ladder that is still at or above min_px. */
int ladder_floor(const int * lad, int min_px)
{
    int last = 0;
    for(int i = 0; i < LADDER_STEPS; i++) if(lad[i] >= min_px) last = i;
    return last;
}

/* Fit the main part of the title in `file` into w x room, in whole words,
 * at the biggest rung down to the floor. Returns the rung, or -1.
 * `big`: the ladder starts a step bigger (short titles fill the tile). */
int fit_main(const char * file, int leading, bool big, int side, int w, int room, int max_lines,
                    const char * main, int min_px, int * lad, fit_t * fm)
{
    int start = big ? side / 4 : side / 6;
    if(start < 16) start = 16;
    if(start > 180) start = 180;
    ladder_fill(lad, start);
    int floor_k = ladder_floor(lad, text_min_px(side));
    int k = fit_rung(file, leading, lad, 0, floor_k, main, w, room, max_lines, fm);
    if(k >= 0) return k;
    if(getenv("SIMPLETON_DEBUG_FIT")) fprintf(stderr, "fit: %s '%s' side %d w %d floor %d (rung %d = %d px) min %d start %d\n", file, main, side, w, text_min_px(side), floor_k, lad[floor_k], min_px, lad[0]);
    /* Below the floor only when a single word is too wide for a line at
     * the floor, and only down to min_px: a word is never broken (Ian,
     * 10 Oct: "Transformer", "Preservation", "Dreamboat" were being cut in
     * two). A title that merely has too many words is trimmed with dots at
     * the floor instead. */
    char tmp[400];
    bool whole;
    wrap_words(face_px(file, lad[floor_k]), main, w, tmp, sizeof(tmp), &whole);
    if(whole) return -1;
    int bottom = ladder_floor(lad, min_px < TEXT_MIN_PX ? TEXT_MIN_PX : min_px);
    if(bottom <= floor_k) return -1;
    return fit_rung(file, leading, lad, floor_k + 1, bottom, main, w, room, max_lines, fm);
}

/* The bracketed tail under a placed main part: whole, three rungs smaller
 * than the main, down the whole ladder (Ian, 10 Oct: "words in a
 * parenthesis you can make as small as you want" - the floor is for the
 * title); else one line at the bottom rung trimmed with dots; else nothing.
 * The extra information packed into some titles "we don't need to know"
 * never shrinks the title. Returns whether there is a tail to show. */
bool fit_tail(const char * file, int leading, int side, int w, int room, int gap, const char * br,
                     const int * lad, int k_main, fit_t * fb)
{
    (void)side;
    int floor_k = LADDER_STEPS - 1;
    int kb = k_main + 3 > floor_k ? floor_k : k_main + 3;
    int tail_room = room - gap / 2;
    if(tail_room <= 0) return false;
    if(fit_rung(file, leading, lad, kb, floor_k, br, w, tail_room, 2, fb) >= 0) return true;
    const lv_font_t * f = face_px(file, lad[floor_k]);
    int lh = lv_font_get_line_height(f);
    if(lh > tail_room) return false;
    fb->font = f; fb->px = lad[floor_k]; fb->whole = false; fb->space = 0;
    snprintf(fb->text, sizeof(fb->text), "%s", br);
    fb->lines = 1; fb->h = lh;
    return true;
}

/* Nothing fits whole: the biggest rung at or above the floor where
 * max_lines lines fit the room (else the floor), wrapped by LVGL, trailing
 * off with dots. */
void fit_fallback(const char * file, int leading, int side, int room, int max_lines, const char * text, bool caps, fit_t * fm)
{
    int lad[LADDER_STEPS];
    int start = side / 6;
    if(start < 16) start = 16;
    ladder_fill(lad, start);
    int floor_k = ladder_floor(lad, text_min_px(side));
    int k = floor_k;
    for(int j = 2; j <= floor_k; j++) {
        const lv_font_t * f = face_px(file, lad[j]);
        if(lines_height(f, max_lines, leading_space(f, leading)) <= room) { k = j; break; }
    }
    fm->font = face_px(file, lad[k]); fm->px = lad[k]; fm->whole = false;
    snprintf(fm->text, sizeof(fm->text), "%s", text);
    if(caps) caps_inplace(fm->text);
    fm->space = leading_space(fm->font, leading);
    int lh = lv_font_get_line_height(fm->font);
    int lines = (room - fm->space) / (lh + fm->space);
    if(lines < 1) lines = 1;
    if(lines > max_lines) lines = max_lines;
    fm->lines = lines; fm->h = lines_height(fm->font, lines, fm->space);
}

