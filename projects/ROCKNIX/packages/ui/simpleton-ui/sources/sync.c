/*
 * SimpletonOS UI - syncing the picture with the sound (implementation).
 * See sync.h. A sheet over everything in the page's colours: the light,
 * the delay under it between arrows, a line of help, the hints.
 */
#include "sync.h"
#include "config.h"
#include "mpdc.h"
#include <sys/stat.h>
#include "layout.h"
#include "saver.h"
#include "strings.h"
#include "theme.h"
#include "viz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STEP_MS   10
#define POLL_MS   16

static lv_obj_t * sheet, * light, * delay_l, * name_l, * help_l, * footer;
static lv_timer_t * timer;
static bool active, feed_ours, lit, metronome;
#define CLICK_DIR_DEFAULT "/var/media/simpleton-sync"    /* MPD mounts folders under /var/media only (patch 0004) */
static const char * click_dir(void) { const char * e = getenv("SIMPLETON_SYNC_DIR"); return e && e[0] ? e : CLICK_DIR_DEFAULT; }   /* the rig */

/* the click track, copied from the share to a folder MPD may mount */
static bool click_ready(void)
{
    char src[600], dst[600];
    snprintf(src, sizeof(src), "%s/sync/click.wav", config_share_dir());
    snprintf(dst, sizeof(dst), "%s/click.wav", click_dir());
    mkdir(click_dir(), 0755);
    FILE * in = fopen(src, "rb"); if(!in) { fprintf(stderr, "simpleton-ui: sync: no %s\n", src); return false; }
    FILE * out = fopen(dst, "wb"); if(!out) { fclose(in); fprintf(stderr, "simpleton-ui: sync: cannot write %s\n", dst); return false; }
    char buf[65536]; size_t n; bool ok = true;
    while((n = fread(buf, 1, sizeof(buf), in)) > 0) if(fwrite(buf, 1, n, out) != n) { ok = false; break; }
    fclose(in); fclose(out);
    return ok;
}
static unsigned last_beats;
static int delay_ms;

static void show_delay(void)
{
    char t[48];
    snprintf(t, sizeof(t), LV_SYMBOL_LEFT " %d ms " LV_SYMBOL_RIGHT, delay_ms);
    lv_label_set_text(delay_l, t);
}

static void poll(lv_timer_t * t)
{
    (void)t;
    unsigned b = viz_beats();
    if(b != last_beats) {
        last_beats = b;
        lit = !lit;
        lv_obj_set_style_bg_color(light, lit ? lv_color_white() : lv_color_black(), 0);
    }
}

void sync_create(void)
{
    sheet = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(sheet);
    lv_obj_set_size(sheet, ui_screen.w, ui_screen.h);
    lv_obj_set_pos(sheet, 0, 0);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
    lv_obj_remove_flag(sheet, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    int d = (ui_screen.w < ui_screen.h ? ui_screen.w : ui_screen.h) * 36 / 100;
    light = lv_obj_create(sheet);
    lv_obj_remove_style_all(light);
    lv_obj_set_size(light, d, d);
    lv_obj_set_style_radius(light, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(light, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(light, lv_color_black(), 0);
    lv_obj_set_style_border_width(light, PX(3), 0);
    lv_obj_set_style_border_opa(light, LV_OPA_COVER, 0);
    lv_obj_align(light, LV_ALIGN_CENTER, 0, -ui_screen.h * 12 / 100);
    name_l = lv_label_create(sheet);
    lv_obj_set_style_text_font(name_l, ui_font_caps(PX(22)), 0);
    delay_l = lv_label_create(sheet);
    lv_obj_set_style_text_font(delay_l, ui_font(36 * UI_GROW_PCT / 100), 0);
    help_l = lv_label_create(sheet);
    lv_obj_set_style_text_font(help_l, ui_font(22), 0);
    lv_obj_set_style_text_align(help_l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(help_l, ui_screen.w * 80 / 100);
    lv_label_set_long_mode(help_l, LV_LABEL_LONG_MODE_WRAP);
    footer = lv_label_create(sheet);
    lv_obj_set_style_text_font(footer, UI_FONT_HINT, 0);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_HIDDEN);
}

void sync_open(void)
{
    if(!sheet || active) return;
    active = true;
    const ui_colours_t * c = ui_colours();
    lv_obj_set_style_bg_color(sheet, lv_color_hex(c->ground), 0);
    lv_obj_set_style_border_color(light, lv_color_hex(c->ink), 0);
    lv_obj_set_style_text_color(name_l, lv_color_hex(c->ink), 0);
    lv_obj_set_style_text_color(delay_l, lv_color_hex(c->sel), 0);
    lv_obj_set_style_text_color(help_l, lv_color_hex(c->ink), 0);
    lv_obj_set_style_text_color(footer, lv_color_hex(ui_mix(c->ground, c->ink, 70)), 0);
    delay_ms = viz_delay_setting();                       /* this output's own (0.37) */
    show_delay();
    { char caps[64]; ui_caps(T(S_SYNC_DELAY), caps, sizeof(caps)); lv_label_set_text(name_l, caps); }
    { char hints[200]; snprintf(hints, sizeof(hints), LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " %s  %s %s", T(S_HINT_CHANGE), input_letter_back(), T(S_HINT_BACK)); lv_label_set_text(footer, hints); }
    int y = ui_screen.h * 6 / 100;
    lv_obj_align_to(name_l, light, LV_ALIGN_OUT_BOTTOM_MID, 0, PX(28));
    lv_obj_align_to(delay_l, name_l, LV_ALIGN_OUT_BOTTOM_MID, 0, PX(4));
    lv_obj_align(help_l, LV_ALIGN_BOTTOM_MID, 0, -y - PX(40));
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -y + PX(8));
    /* the metronome through MPD (the path being measured), the queue put aside */
    metronome = click_ready() && mpd_sync_begin(click_dir());
    if(!metronome) fprintf(stderr, "simpleton-ui: sync: no metronome - whatever plays will have to do\n");
    /* the feed: on for the page unless a visualizer already has it */
    feed_ours = !viz_active() && viz_start(VIZ_FEED);
    lv_label_set_text(help_l, T(metronome ? S_SYNC_HELP : S_SYNC_HELP_MUSIC));
    viz_set_delay_ms(delay_ms);
    last_beats = viz_beats(); lit = false;
    lv_obj_set_style_bg_color(light, lv_color_black(), 0);
    lv_obj_remove_flag(sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(sheet);
    if(!timer) timer = lv_timer_create(poll, POLL_MS, NULL);
    lv_timer_resume(timer);
    fprintf(stderr, "simpleton-ui: sync: open, delay %d ms%s\n", delay_ms, feed_ours ? "" : " (a visualizer's feed)");
}

void sync_close(void)
{
    if(!active) return;
    active = false;
    if(timer) lv_timer_pause(timer);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_HIDDEN);
    if(feed_ours) viz_stop();
    feed_ours = false;
    if(metronome) { mpd_sync_end(); metronome = false; }
    fprintf(stderr, "simpleton-ui: sync: delay %d ms\n", delay_ms);
}

bool sync_active(void) { return active; }

void sync_handle_action(ui_action_t a)
{
    switch(a) {
        case ACT_LEFT:
        case ACT_RIGHT: {
            int n = delay_ms + (a == ACT_RIGHT ? STEP_MS : -STEP_MS);
            if(n < 0) n = 0;
            if(n > 1000) n = 1000;
            if(n == delay_ms) return;
            delay_ms = n;
            char v[16]; snprintf(v, sizeof(v), "%d", delay_ms);
            config_write(viz_delay_key(), v);
            viz_set_delay_ms(delay_ms);                      /* live */
            show_delay();
            return;
        }
        case ACT_BACK:
        case ACT_SELECT:
        case ACT_SETTINGS:
            sync_close();
            return;
        default:
            return;
    }
}
