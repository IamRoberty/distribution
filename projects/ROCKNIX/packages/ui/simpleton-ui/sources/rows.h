/*
 * SimpletonOS UI - the Artists rows view (0.20, 14 Oct 2026; Note 05 step
 * 8, "View 1, Netflix style" in browse-views-ian-2026-10-13.md).
 *
 * One row per artist: their name tile (or picture) pinned at the left,
 * their albums in year order running off to the right. Rows are the
 * grid's sizes (the picker's Size: a row is one grid cell tall, the
 * screen holds the grid's rows and columns), the covers the grid's tiles.
 *
 * Navigation (settled with Ian, 13 Oct):
 *   - Right walks the selection to the row's centre slot; past it the
 *     selection stays and the row slides under it; once the last album is
 *     on screen the selection walks on to the last slot. Left mirrors it:
 *     back to the centre, the row slides back until album 1 is beside the
 *     artist tile, then on to the artist tile.
 *   - Right on a row's last album pages down a screenful of artists (to
 *     the centre slot, or the row's last album when it doesn't reach the
 *     centre); Left on the artist tile pages up (to the artist tile).
 *   - Up/Down keep your column before the centre; past it you land on the
 *     centre slot when the new row slides, else stay in your column or on
 *     the row's last album. The column aimed for is remembered across
 *     Up/Down; Left/Right reset it.
 *   - A row keeps its scroll while it stays on screen; off screen it
 *     resets, so it comes back showing album 1 beside the artist.
 *   - Select on an album opens it (the page); on the artist tile, what
 *     the "Opens as" row says (main.c). Back leaves. The pill jumps by the
 *     artist's letter.
 */
#ifndef SIMPLETON_ROWS_H
#define SIMPLETON_ROWS_H

#include "lvgl.h"
#include "input.h"
#include "library.h"
#include "tilestyle.h"
#include <stdbool.h>

/* `on_artist`: Select on an artist tile (the artists.h index). `on_album`:
 * Select on a cover, as the grid's. `on_exit`: Back. `on_style`: the
 * picker chose another face ("grid" / "list"). */
void rows_create(void (*on_artist)(int artist),
                 void (*on_album)(const library_t * lib, int album, const char * card, const tile_style_t * tile),
                 void (*on_exit)(void), void (*on_style)(const char * style));

void rows_show(bool fresh);
void rows_hide(void);
void rows_handle_action(ui_action_t a);
bool rows_overlay_active(void);
void rows_toggle_picker(void);
void rows_show_picker_style(void);
void rows_reload_options(void);
void rows_library_changed(void);

/* Put the cursor on an artist's tile (coming back from their albums). */
bool rows_focus_artist(int artist);
int  rows_selected_artist(void);

#endif
