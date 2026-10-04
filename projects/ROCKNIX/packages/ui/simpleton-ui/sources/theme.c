/*
 * SimpletonOS UI - stage helper. See theme.h.
 *
 * ui_font() lives in fonts.c (0.9); the screen and stage sizes live in
 * layout.c (0.11).
 */
#include "theme.h"

lv_obj_t * ui_stage_create(lv_obj_t * screen)
{
    lv_obj_t * s = lv_obj_create(screen);
    lv_obj_remove_style_all(s);                 /* no bg, border, padding */
    lv_obj_set_size(s, ui_screen.stage, ui_screen.stage);
    lv_obj_center(s);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE);
    return s;
}
