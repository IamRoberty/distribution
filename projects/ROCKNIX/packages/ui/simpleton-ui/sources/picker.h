/*
 * SimpletonOS UI - the browse picker and the jump pill (0.15, 7 Oct 2026;
 * Engineering Note 05 section 3, build step 5, reshaped after Ian's first
 * hardware pass of 0.14).
 *
 * Two faces of one thing, drawn over a browse view on the stage:
 *
 *   The PANEL - a compact card centred on the stage, four "< value >"
 *   rows: view style, size, sort, alphabet picker (on/off). Up/Down moves
 *   between rows, Left/Right changes a value - and the change applies to
 *   the view behind at once (the caller's on_change), nothing to confirm.
 *   Select means "that one": it moves on to the next row, and from the
 *   last row closes the panel. Back, or the Settings button again, closes
 *   it. It opens the first time a view is visited on a screen type, from
 *   the Settings button on a browse view, and from the pill with Up.
 *
 *   The PILL - "< A >" top centre over the list: the current jump letter.
 *   Left/Right changes it (wrapping) and the list behind follows at once;
 *   Select or Down keeps the place and closes (Down: into the list
 *   below); Up opens the panel; Back closes and leaves the view (the
 *   caller's Back). It appears on entering a long listing when the view's
 *   alphabet picker is on - never on the way back up.
 *
 * Jump labels are built from what is in the list, never a fixed A-Z:
 * the first letter of each name, folded (case and accents, so "Édith"
 * sits under E), numbers and punctuation under "#", grouped Latin, then
 * Cyrillic, then #, then any other script by code point. "?" at the end
 * stands for the view's end section (items without the tag the sort
 * needs) when the view has one.
 *
 * A shared screen: the theme's colours and fonts only, the same rows as
 * Settings, so it reads the same under every theme.
 */
#ifndef SIMPLETON_PICKER_H
#define SIMPLETON_PICKER_H

#include "lvgl.h"
#include "input.h"
#include "layout.h"
#include <stdbool.h>

#define PICKER_MAX_CHOICES 6
#define PICKER_MAX_LABELS  96
#define PICKER_LABEL_LEN   8          /* one UTF-8 letter, or "#" / "?" */

typedef struct {
    int          style, style_count;
    const char * styles[PICKER_MAX_CHOICES];     /* translated names */
    ui_size_t    size;
    int          sort, sort_count;
    const char * sorts[PICKER_MAX_CHOICES];
    bool         strip;                           /* the alphabet picker (pill) is on for this view */
    int          jump;                            /* index into labels, -1 = none */
    int          label_count;
    char         labels[PICKER_MAX_LABELS][PICKER_LABEL_LEN];
} picker_opts_t;

typedef enum { PICKER_NONE, PICKER_STRIP, PICKER_FULL } picker_mode_t;

/* Put the (hidden) objects on a view's stage: built on the first call,
 * moved to the new stage and closed after that - one picker serves every
 * browse view, whichever is on screen. `lm` is the view's list area (the
 * pill sits in its top row band, the panel centred on it); call
 * picker_relayout() when it changes. The faces are drawn at the menu size,
 * not the view's. */
void picker_attach(lv_obj_t * stage, const ui_list_t * lm);
void picker_relayout(const ui_list_t * lm);

/* Open as the pill or the panel. `on_jump` fires as the pill's letter
 * changes (the list follows live); `on_change` on every value change in
 * the panel (apply it, remember it); `on_close` once, when it closes:
 * `back` is true when Back closed it. */
void picker_open(picker_mode_t mode, const picker_opts_t * opts,
                 void (*on_jump)(int label, void * ctx),
                 void (*on_change)(const picker_opts_t * opts, void * ctx),
                 void (*on_close)(bool back, void * ctx), void * ctx);
void picker_close(void);
picker_mode_t picker_mode(void);
const picker_opts_t * picker_opts(void);        /* the values while open */

/* The Settings button on a browse view: panel up -> close it; pill up ->
 * the panel. (With nothing up the caller opens the panel itself.) */
void picker_toggle_panel(void);

/* Every action while open. */
void picker_handle_action(ui_action_t a);

/* ---- labels ---- */

/* The jump label a name sorts under (see the header comment). */
void picker_fold(const char * name, char out[PICKER_LABEL_LEN]);

/* Fill opts->labels from a list of names: each distinct folded letter
 * once, in group order. `end_section` adds the "?" chip. */
void picker_labels_from_names(picker_opts_t * opts, const char * const * names, int count, bool end_section);

/* Index of the label `name` sorts under in opts->labels, or -1. */
int picker_label_of(const picker_opts_t * opts, const char * name);

#endif
