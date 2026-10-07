/*
 * SimpletonOS UI - the tape shelf view style (0.13, 7 Oct 2026).
 *
 * A row of cassette spines standing side by side on a walnut shelf (Ian's
 * Archer Ave. design, 4-7 Oct 2026). One item = one tape: the theme supplies
 * the spine art (a 180x1080 PNG with the label area left blank) and a text
 * rule (typeface, size, case, ink), and the view writes the item's name on
 * the label in that rule, so a tape reads in whatever language the UI is
 * set to.
 *
 *   - The ring is endless: a half tape shows at each edge, and left from
 *     the first tape wraps to the last. The selected tape sits at the
 *     centre; the shelf slides under it, a short eased move per press.
 *   - The selected tape is "pulled out": drawn larger than its neighbours,
 *     in front of the wood rails, with a shadow. The rest size and the
 *     pulled size are theme settings (fractions of the master art), so the
 *     pulled tape is the sharpest rendition and nothing is enlarged.
 *   - Orientation follows the screen: wider than 4:3 is a bookshelf
 *     (spines vertical, text along the spine, Left/Right moves, rails top
 *     and bottom); square and portrait screens get a stack (the same art
 *     turned on its side, text horizontal, Up/Down moves, rails left and
 *     right). One set of art serves both.
 *   - Every spine is composed once, when the items are set (and again on a
 *     language change): a downscaled tile plus the label text, as plain
 *     ARGB images. Scrolling is then a blit; nothing is rendered per frame
 *     but the slide.
 *
 * Text sizes in the rule are POINTS AT THE 1080 MASTER, as Ian's Krita
 * document measures them: 1 pt = 4/3 px. 72 pt is a 96 px em on a
 * 180x1080 tile, and scales with the tape.
 *
 * The view owns no meaning: the Home page (home.c) decides what the tapes
 * are and what selecting one does. Other views may use it later.
 */
#ifndef SIMPLETON_SHELF_H
#define SIMPLETON_SHELF_H

#include "lvgl.h"
#include "input.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum { SHELF_CASE_AS_IS, SHELF_CASE_UPPER, SHELF_CASE_LOWER, SHELF_CASE_TITLE } shelf_case_t;
typedef enum { SHELF_ALIGN_CENTRE, SHELF_ALIGN_START, SHELF_ALIGN_END } shelf_align_t;

typedef struct {
    char          tile[128];     /* spine PNG in the theme's asset folder   */
    char          font[128];     /* typeface file in <share>/fonts          */
    int           size_pt;       /* at the 1080 master, 1 pt = 4/3 px       */
    shelf_case_t  letter_case;
    shelf_align_t align;         /* along the spine                          */
    uint32_t      ink;           /* 0xRRGGBB                                 */
    const char *  label;         /* the name to write (already translated)  */
} shelf_item_t;

typedef struct {
    double   rest;               /* resting tape, fraction of the master (0.889 = 960 of 1080) */
    double   pull;               /* pulled-out tape (0.96)                    */
    int      rail;               /* wood rail thickness at the master (60)    */
    char     wood_h[128];        /* long-grain strip, tiles along the shelf   */
    char     wood_v[128];        /* short-grain strip, tiles down the stack   */
    uint32_t back;               /* colour behind the tapes, 0xRRGGBB         */
    int      shadow;             /* shadow width of the pulled tape at the master (0 = none) */
} shelf_style_t;

/* Fill a style with the defaults above (no wood, dark back). */
void shelf_style_defaults(shelf_style_t * st);

/* Create the view, the size of the whole screen, on `parent`. */
lv_obj_t * shelf_create(lv_obj_t * parent, const shelf_style_t * st);

/* Compose the spines for `items` (copied) and show them, `selected` in the
 * centre. Returns false when nothing could be composed (no usable tile
 * at all); the caller then shows a plain list instead. Items whose tile
 * is missing get a flat tape in the back colour with the label on it. */
bool shelf_set_items(lv_obj_t * shelf, const shelf_item_t * items, int count, int selected);

/* Move the selection by `dir` (+1 forward, -1 back; wraps), with the slide. */
void shelf_move(lv_obj_t * shelf, int dir);

/* Jump to an item without the slide. */
void shelf_select(lv_obj_t * shelf, int index);

/* Take an item off the shelf or put it back (its spine stays composed, so
 * this is instant). A hidden selected item hands the selection to its
 * next visible neighbour. At least one item is always visible. */
void shelf_set_visible(lv_obj_t * shelf, int index, bool visible);

int shelf_selected(const lv_obj_t * shelf);
int shelf_count(const lv_obj_t * shelf);

/* True when this screen gets the bookshelf (wide); false = the stack. */
bool shelf_is_wide(void);

/* The navigation keys of the orientation in use: returns true when the
 * action moved the selection (UP/DOWN on a stack, LEFT/RIGHT on a shelf).
 * Everything else is left to the caller. */
bool shelf_handle_action(lv_obj_t * shelf, ui_action_t a);

#endif
