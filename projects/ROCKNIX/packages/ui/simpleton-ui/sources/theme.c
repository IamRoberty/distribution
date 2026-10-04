/*
 * SimpletonOS UI - stage scaling helpers. See theme.h.
 *
 * ui_font() moved to fonts.c (0.9): fonts are FreeType chains at the exact
 * scaled size, not the nearest built-in Montserrat.
 */
#include "theme.h"

int ui_base = 720;

lv_obj_t * ui_stage_create(lv_obj_t * screen)
{
    lv_obj_t * s = lv_obj_create(screen);
    lv_obj_remove_style_all(s);                 /* no bg, border, padding */
    lv_obj_set_size(s, ui_base, ui_base);
    lv_obj_center(s);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE);
    return s;
}
