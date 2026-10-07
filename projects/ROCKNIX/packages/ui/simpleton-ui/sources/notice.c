/*
 * SimpletonOS UI - a notice screen (implementation). See notice.h.
 */
#include "notice.h"
#include "strings.h"
#include "theme.h"

#include <stdio.h>

static lv_obj_t * scr, * title, * body, * footer;

void notice_create(void)
{
    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_t * stage = ui_stage_create(scr);

    ui_list_t lm;
    ui_list_metrics(UI_SIZE_DEFAULT, &lm);

    title = lv_label_create(stage);
    lv_obj_set_width(title, lm.w);
    lv_obj_set_pos(title, lm.x, lm.y + lm.row_h);
    lv_obj_set_style_text_font(title, ui_font_px(lm.font_px * 3 / 2), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_FG), 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_WRAP);

    body = lv_label_create(stage);
    lv_obj_set_width(body, lm.w);
    lv_obj_set_pos(body, lm.x, lm.y + lm.row_h * 3);
    lv_obj_set_style_text_font(body, ui_font_px(lm.font_px), 0);
    lv_obj_set_style_text_color(body, lv_color_hex(UI_COLOR_DIM), 0);
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);

    footer = lv_label_create(stage);
    lv_obj_set_size(footer, lm.w, lm.footer_h);
    lv_obj_set_pos(footer, lm.x, lm.y + lm.h);
    lv_obj_set_style_text_font(footer, ui_font_px(lm.chrome_font_px), 0);
    lv_obj_set_style_text_color(footer, lv_color_hex(UI_COLOR_DIM), 0);
    char hints[100];
    snprintf(hints, sizeof(hints), "B %s", T(S_HINT_BACK));
    lv_label_set_text(footer, hints);
}

void notice_show(const char * t, const char * text)
{
    lv_label_set_text(title, t ? t : "");
    lv_label_set_text(body, text ? text : "");
    lv_screen_load(scr);
}

void notice_hide(void) {}

bool notice_handle_action(ui_action_t a)
{
    return a == ACT_BACK || a == ACT_SELECT;
}
