/*
 * SimpletonOS UI - the liner sheet (0.38, 25 Oct 2026; Ian's design from
 * his ruled-paper sheets, 24-25 Oct). A list written on a sheet of ruled
 * paper: 14 rules, seven tracks a page, each on two rules - the artist
 * small on the upper one, the title full size on the lower, a bracket tail
 * ("(Live)", "[Remaster]") at 70 % of the title - the track numbers in the
 * margin on some sheets (dealt), the selected track in a colour from the
 * paper's own palette set, the playing one with a small triangle in the
 * margin. The paper is a light palette colour over white (a multiply at
 * 80 %), the rules grey-blue, the double margin line pink. A mixtape's
 * sheet is in the mixtape's own hand (tilestyle_hand_for); anything else in
 * a liner face dealt per sheet.
 *
 * Two users (0.38a): the mixtape's page in listpage.c - its track list IS
 * the sheet, beside the cassette on the TV, the whole screen on the
 * handheld, Up/Down along it, Left/Right a page of seven - and the Liner
 * notes screensaver (saver.c), which writes the queue and turns the page
 * when the playing track leaves it.
 */
#ifndef SIMPLETON_LINER_H
#define SIMPLETON_LINER_H

#include "lvgl.h"
#include "tilestyle.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct liner_sheet liner_sheet_t;
typedef struct { const char * title; const char * artist; } liner_row_t;

liner_sheet_t * liner_sheet_create(lv_obj_t * parent);
void            liner_sheet_delete(liner_sheet_t * s);
void            liner_sheet_place(liner_sheet_t * s, int x, int y, int w, int h);
/* Deal the paper, its inks and the numbers from `seed`; the face: a hand
 * (file, case, size %) or, with `hand` NULL, a liner face dealt too. */
void            liner_sheet_deal(liner_sheet_t * s, unsigned seed, const char * hand, hand_case_t letter_case, int size_pct);
/* Write rows first..first+6 of `rows`; `selected` and `playing` are row
 * indexes (-1 = none). Without a selection the playing row takes the
 * colour (the saver). */
void            liner_sheet_write(liner_sheet_t * s, const liner_row_t * rows, int n, int first, int selected, int playing);
void            liner_sheet_show(liner_sheet_t * s, bool on);
uint32_t        liner_sheet_paper(const liner_sheet_t * s);
uint32_t        liner_sheet_ink(const liner_sheet_t * s);

/* The screensaver (saver.c hosts it). */
void liner_create(lv_obj_t * parent);              /* once; `parent` is the saver's root */
void liner_start(int x, int w);                    /* the sheet's place (full height); deals the paper, writes the page */
void liner_stop(void);
void liner_mpd_changed(void);                      /* the queue or the playing track changed: the page follows */
bool liner_active(void);

#endif
