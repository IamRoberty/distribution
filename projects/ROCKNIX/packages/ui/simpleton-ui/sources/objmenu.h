/*
 * SimpletonOS UI - the object menu (0.21, 15 Oct 2026): A held on a track
 * or an album, the TV remote's Options key or OK held (cec.c).
 *
 * A small box over whatever is on screen, in the theme's colours and
 * fonts (a shared screen): for a track Favorite (or Remove from
 * Favorites) and Add to mixtape...; for an album Add to collection... and
 * Add to mixtape... (all its tracks); inside a mixtape or collection also
 * Remove from <name>; on a mixtape or collection row Delete <name>.
 * "Add to..." opens a second list: "+ New mixtape" first, then the
 * mixtapes most recently changed first (the last one used selected). A
 * new one is named "Mixtape 1" / "Collection 1" at once (the first unused
 * number), so adding never waits for typing. Up/Down, Select, Back out.
 * What happened shows as a toast at the bottom for a moment.
 *
 * The store is curation.h; the views are told through `on_changed` so
 * hearts and lists follow.
 */
#ifndef SIMPLETON_OBJMENU_H
#define SIMPLETON_OBJMENU_H

#include "input.h"
#include <stdbool.h>

void objmenu_create(void (*on_changed)(void));

/* A track: its URI and title (the artist for the box's header). `in_uid`
 * / `in_pos`: the mixtape it is being looked at in, else 0 / -1. */
void objmenu_open_track(const char * uri, const char * title, const char * artist, int in_uid, int in_pos);
/* An album: its folder, title and artist, and its tracks' URIs. */
void objmenu_open_album(const char * folder, const char * title, const char * artist, const char * const * uris, int n, int in_uid, int in_pos);
/* A mixtape or collection row (the Mixtapes / Collections lists): Delete. */
void objmenu_open_list(int uid);
/* Now Playing's menu (0.43): the track's menu with "Reset DAC" on it. */
void objmenu_open_np(const char * uri, const char * title, const char * artist);

bool objmenu_active(void);
void objmenu_handle_action(ui_action_t a);
/* A choice pop-up (0.42): a sign with `title` in caps, `hint` under it and
 * one "< name >" row; Left/Right between `names`, Select closes and calls
 * `on_pick` with the index, Back closes with nothing done. `names` must
 * outlive the pop-up. */
void objmenu_open_choice(const char * title, const char * hint, const char * const * names, int n, int current, void (*on_pick)(int index, void * ctx), void * ctx);

/* The favourite heart (0.21a): a small red heart `r` wide (half-width) as
 * an image object, for a row or a cell; the bitmap is shared per size. */
#include "lvgl.h"
lv_obj_t * curation_heart(lv_obj_t * parent, int r, uint32_t colour);
const lv_image_dsc_t * curation_heart_dsc(int r, uint32_t colour);

/* A short message at the bottom of the screen (also for others). */
void objmenu_toast(const char * text);

#endif
