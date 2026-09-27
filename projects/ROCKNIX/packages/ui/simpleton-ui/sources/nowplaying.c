/*
 * SimpletonOS UI - now-playing screen (implementation). See nowplaying.h.
 *
 * Layout (720x720): the art fills the panel edge to edge (album covers are
 * square, the panel is square). A gradient strip over the bottom 340 px
 * carries title / artist / format line, then the three control rows:
 *
 *   row 0  scrub bar         left/right = seek +/-10 s
 *   row 1  transport         left/right moves between prev / play / next
 *   row 2  favorite/info/more (placeholders until curation lands)
 *
 * Status comes from MPD twice a second, so a change made from any other
 * control surface (a phone app later) shows here within half a second.
 *
 * The format line tells the truth from two sides: what MPD decoded (its
 * `audio:` field) and what the kernel is actually sending the DAC, read
 * from /proc/asound/card*\/stream0 - the file that exposed the FC4's silent
 * PCM fallback on 27 Sep. MPD alone would have said "DSD64" while the DAC
 * got PCM. Cards are matched by "which one is running", never by index.
 */
#include "nowplaying.h"
#include "art.h"
#include "mpdc.h"
#include "theme.h"

#include <ctype.h>
#include <glob.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define STATUS_POLL_MS   500
#define ART_POLL_MS      100
#define IDLE_HIDE_MS     4000
#define FADE_MS          250
#define SEEK_STEP_S      10.0f

#define OVERLAY_H        340
#define FONT_TITLE       (&lv_font_montserrat_36)

enum { ROW_SCRUB = 0, ROW_TRANSPORT, ROW_BOTTOM, ROW_COUNT };
enum { TR_PREV = 0, TR_PLAY, TR_NEXT, TR_COUNT };
enum { BT_FAV = 0, BT_INFO, BT_MORE, BT_COUNT };

static lv_obj_t * scr;
static lv_obj_t * art_img;
static lv_obj_t * placeholder;        /* shown when there is no art       */
static lv_obj_t * placeholder_letter;
static lv_obj_t * overlay;
static lv_obj_t * title_lbl, * artist_lbl, * format_lbl;
static lv_obj_t * bar, * knob, * elapsed_lbl, * duration_lbl;
static lv_obj_t * tr_btn[TR_COUNT], * tr_lbl[TR_COUNT];
static lv_obj_t * bt_btn[BT_COUNT];

static lv_timer_t * status_timer, * art_timer, * idle_timer;

static bool active;
static bool controls_visible;
static int  row = ROW_TRANSPORT, tr_cur = TR_PLAY, bt_cur = BT_FAV;

static mpd_status_t st;
static char cur_file[1024];           /* file the art on screen belongs to */
static lv_image_dsc_t art_dsc[2];     /* alternate so LVGL sees a new src */
static int art_slot;
static uint32_t last_dac_read_ms;
static char dac_line[96];

/* ------------------------------------------------------- format line */

/* "44100:16:2" / "dsd64:2" -> "44.1 kHz • 16-bit" / "DSD64" (U+2022 is in the built-in fonts; U+00B7 is not) */
static void describe_source(const char * audio, char * out, size_t len)
{
    if(strncmp(audio, "dsd", 3) == 0) { snprintf(out, len, "DSD%d", atoi(audio + 3)); return; }
    int rate = atoi(audio);
    const char * colon = strchr(audio, ':');
    if(rate <= 0 || !colon) { out[0] = 0; return; }
    const char * bits = colon + 1;
    char rate_s[16];
    if(rate % 1000 == 0) snprintf(rate_s, sizeof(rate_s), "%d", rate / 1000);
    else                 snprintf(rate_s, sizeof(rate_s), "%.1f", rate / 1000.0);
    if(*bits == 'f')     snprintf(out, len, "%s kHz \xE2\x80\xA2 float", rate_s);
    else                 snprintf(out, len, "%s kHz \xE2\x80\xA2 %d-bit", rate_s, atoi(bits));
}

static const char * codec_name(const char * file)
{
    const char * cont = NULL;
    for(const char * p = file; *p; p++) {
        if(*p == '.' && (strncasecmp(p, ".iso/", 5) == 0)) return "SACD";
        if(*p == '.' && (strncasecmp(p, ".cue/", 5) == 0)) cont = p;
    }
    if(cont) return "CUE";
    const char * dot = strrchr(file, '.');
    if(!dot) return "";
    static const struct { const char * ext, * name; } map[] = {
        { "flac", "FLAC" }, { "dsf", "DSF" }, { "dff", "DFF" }, { "mp3", "MP3" }, { "wav", "WAV" },
        { "ape", "APE" }, { "wv", "WavPack" }, { "m4a", "AAC" }, { "ogg", "Ogg" }, { "opus", "Opus" },
        { "aiff", "AIFF" }, { "aif", "AIFF" }, { "alac", "ALAC" }, { NULL, NULL } };
    for(int i = 0; map[i].ext; i++) if(strcasecmp(dot + 1, map[i].ext) == 0) return map[i].name;
    return "";
}

static const char * nearest_rate(double f)
{
    static const struct { double hz; const char * s; } rates[] = {
        { 44100, "44.1" }, { 48000, "48" }, { 88200, "88.2" }, { 96000, "96" }, { 176400, "176.4" }, { 192000, "192" },
        { 352800, "352.8" }, { 384000, "384" }, { 705600, "705.6" }, { 768000, "768" } };
    const char * best = "?";
    double best_d = 1e12;
    for(size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        double d = fabs(rates[i].hz - f);
        if(d < best_d) { best_d = d; best = rates[i].s; }
    }
    return best;
}

/*
 * Parse one /proc/asound/cardN/stream0. Returns true when its Playback side
 * is running and fills `out` with e.g. "FC4: DSD64 native".
 *
 * Shape of the file (verified on the FC4, 27 Sep 2026):
 *   HiBy FC4 at usb-fd800000.usb-1, high speed : USB Audio
 *   Playback:
 *     Status: Running
 *       Interface = 1
 *       Altset = 4
 *       Momentary freq = 88192 Hz (0x58.0f)
 *       DOP = 0
 *     Interface 1
 *       Altset 1
 *       Format: S32_LE
 *       ...
 *     Interface 1
 *       Altset 4
 *       Format: DSD_U32_BE
 *   Capture: ...
 */
static bool parse_stream0(const char * path, char * out, size_t len)
{
    FILE * f = fopen(path, "r");
    if(!f) return false;
    char line[256], name[64] = "DAC", fmt[32] = "";
    int running_alt = -1, dop = 0, cur_alt = -1;
    double freq = 0;
    bool in_playback = false, running = false, first = true;

    while(fgets(line, sizeof(line), f)) {
        if(first) {
            first = false;
            /* "HiBy FC4 at usb-..." -> "HiBy FC4" */
            char * at = strstr(line, " at ");
            if(at) { *at = 0; snprintf(name, sizeof(name), "%.60s", line); }
            continue;
        }
        if(strncmp(line, "Playback:", 9) == 0) { in_playback = true; continue; }
        if(strncmp(line, "Capture:", 8) == 0) break;
        if(!in_playback) continue;

        char * s = line;
        while(*s == ' ') s++;
        if(strncmp(s, "Status: Running", 15) == 0) running = true;
        else if(strncmp(s, "Altset = ", 9) == 0) running_alt = atoi(s + 9);
        else if(strncmp(s, "Momentary freq = ", 17) == 0) freq = atof(s + 17);
        else if(strncmp(s, "DOP = ", 6) == 0) dop = atoi(s + 6);
        else if(strncmp(s, "Altset ", 7) == 0 && isdigit((unsigned char)s[7])) cur_alt = atoi(s + 7);
        else if(strncmp(s, "Format: ", 8) == 0 && cur_alt == running_alt && !fmt[0]) {
            snprintf(fmt, sizeof(fmt), "%s", s + 8);
            fmt[strcspn(fmt, "\n")] = 0;
        }
    }
    fclose(f);
    if(!running) return false;

    if(strncmp(fmt, "DSD_U32", 7) == 0 || strncmp(fmt, "DSD_U16", 7) == 0 || strncmp(fmt, "DSD_U8", 6) == 0) {
        int bits = strncmp(fmt, "DSD_U32", 7) == 0 ? 32 : (strncmp(fmt, "DSD_U16", 7) == 0 ? 16 : 8);
        int mult = (int)lround(freq * bits / 44100.0);
        snprintf(out, len, "%s: DSD%d native", name, mult);
    }
    else if(dop) {
        snprintf(out, len, "%s: DSD over PCM %s kHz", name, nearest_rate(freq));
    }
    else if(fmt[0]) {
        snprintf(out, len, "%s: PCM %s kHz", name, nearest_rate(freq));
    }
    else {
        snprintf(out, len, "%s: running", name);
    }
    return true;
}

static void refresh_dac_line(void)
{
    glob_t g;
    dac_line[0] = 0;
    if(glob("/proc/asound/card*/stream0", 0, NULL, &g) == 0) {
        for(size_t i = 0; i < g.gl_pathc && !dac_line[0]; i++) parse_stream0(g.gl_pathv[i], dac_line, sizeof(dac_line));
    }
    globfree(&g);
}

static void update_format_line(void)
{
    if(!st.file[0]) { lv_label_set_text(format_lbl, ""); return; }
    uint32_t now = lv_tick_get();
    if(now - last_dac_read_ms > 1500 || last_dac_read_ms == 0) { refresh_dac_line(); last_dac_read_ms = now; }

    char src[48];
    describe_source(st.audio, src, sizeof(src));
    const char * codec = codec_name(st.file);
    char text[200];
    if(dac_line[0]) snprintf(text, sizeof(text), "%s %s   " LV_SYMBOL_RIGHT "   %s", codec, src, dac_line);
    else if(strcmp(st.state, "play") == 0) snprintf(text, sizeof(text), "%s %s", codec, src);
    else            snprintf(text, sizeof(text), "%s %s   " LV_SYMBOL_RIGHT "   DAC idle", codec, src);
    lv_label_set_text(format_lbl, text);
}

/* ------------------------------------------------------------ visuals */

static void fmt_time(float s, char * out, size_t len)
{
    int t = (int)(s + 0.5f);
    if(t < 0) t = 0;
    if(t >= 3600) snprintf(out, len, "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    else          snprintf(out, len, "%d:%02d", t / 60, t % 60);
}

static void style_focus_pill(lv_obj_t * o, bool focused)
{
    lv_obj_set_style_bg_opa(o, focused ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(UI_COLOR_FOCUS_BG), 0);
    lv_obj_t * l = lv_obj_get_child(o, 0);
    if(l) lv_obj_set_style_text_color(l, lv_color_hex(focused ? UI_COLOR_FOCUS_FG : UI_COLOR_FG), 0);
}

static void apply_focus(void)
{
    bool scrub = row == ROW_SCRUB;
    lv_obj_set_style_bg_color(bar, lv_color_hex(scrub ? UI_COLOR_FOCUS_BG : UI_COLOR_FG), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(scrub ? 0x4A5566 : 0x3A404A), LV_PART_MAIN);
    if(scrub) lv_obj_remove_flag(knob, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(knob, LV_OBJ_FLAG_HIDDEN);

    for(int i = 0; i < TR_COUNT; i++) style_focus_pill(tr_btn[i], row == ROW_TRANSPORT && i == tr_cur);
    for(int i = 0; i < BT_COUNT; i++) style_focus_pill(bt_btn[i], row == ROW_BOTTOM && i == bt_cur);
}

static void place_knob(void)
{
    int32_t w = lv_obj_get_width(bar);
    int32_t range = lv_bar_get_max_value(bar);
    int32_t x = range > 0 ? (int32_t)((int64_t)lv_bar_get_value(bar) * w / range) : 0;
    lv_obj_set_pos(knob, lv_obj_get_x(bar) + x - 10, lv_obj_get_y(bar) + lv_obj_get_height(bar) / 2 - 10);
}

static void update_transport_icon(void)
{
    lv_label_set_text(tr_lbl[TR_PLAY], strcmp(st.state, "play") == 0 ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

static void update_text(void)
{
    if(!st.file[0]) {
        lv_label_set_text(title_lbl, "Nothing playing");
        lv_label_set_text(artist_lbl, "Pick a track in the browser");
        return;
    }
    lv_label_set_text(title_lbl, st.title);
    char sub[600];
    if(st.artist[0] && st.album[0]) snprintf(sub, sizeof(sub), "%s  \xE2\x80\xA2  %s", st.artist, st.album);
    else snprintf(sub, sizeof(sub), "%s", st.artist[0] ? st.artist : st.album);
    lv_label_set_text(artist_lbl, sub);
}

static void update_progress(void)
{
    char e[16], d[16];
    fmt_time(st.elapsed, e, sizeof(e));
    fmt_time(st.duration, d, sizeof(d));
    lv_label_set_text(elapsed_lbl, e);
    lv_label_set_text(duration_lbl, st.duration > 0 ? d : "--:--");
    int32_t max = st.duration > 0 ? (int32_t)(st.duration * 10) : 1;
    lv_bar_set_range(bar, 0, max);
    lv_bar_set_value(bar, st.duration > 0 ? (int32_t)(st.elapsed * 10) : 0, LV_ANIM_OFF);
    place_knob();
}

static void show_placeholder(void)
{
    lv_obj_add_flag(art_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(placeholder, LV_OBJ_FLAG_HIDDEN);
    const char * seed = st.album[0] ? st.album : (st.artist[0] ? st.artist : st.title);
    char letter[8] = "";
    if(seed && seed[0]) {
        /* first UTF-8 character, uppercased when ASCII */
        size_t n = 1;
        while(seed[n] && (seed[n] & 0xC0) == 0x80 && n < 4) n++;
        snprintf(letter, sizeof(letter), "%.*s", (int)n, seed);
        if(n == 1) letter[0] = (char)toupper((unsigned char)letter[0]);
    }
    lv_label_set_text(placeholder_letter, letter);
}

static void set_art(const art_result_t * r)
{
    art_slot ^= 1;
    lv_image_dsc_t * d = &art_dsc[art_slot];
    uint8_t * old_pixels = (uint8_t *)d->data;

    memset(d, 0, sizeof(*d));
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_XRGB8888;
    d->header.w = (uint32_t)r->w;
    d->header.h = (uint32_t)r->h;
    d->header.stride = (uint32_t)r->w * 4;
    d->data_size = (uint32_t)r->w * (uint32_t)r->h * 4;
    d->data = r->pixels;

    lv_image_set_src(art_img, d);
    lv_obj_center(art_img);
    lv_obj_remove_flag(art_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(placeholder, LV_OBJ_FLAG_HIDDEN);

    /* the previous slot is no longer referenced by the image object */
    free(old_pixels);
}

/* ------------------------------------------------------------- timers */

static void art_timer_cb(lv_timer_t * t)
{
    (void)t;
    bool found;
    art_result_t r;
    if(!art_poll(&found, &r)) return;
    if(found) set_art(&r);
    else show_placeholder();
}

static void status_timer_cb(lv_timer_t * t)
{
    (void)t;
    mpd_status_t s;
    bool ok = mpd_status(&s);
    if(!ok) { memset(&s, 0, sizeof(s)); s.songid = -1; }
    bool track_changed = strcmp(s.file, cur_file) != 0;
    st = s;

    if(track_changed) {
        snprintf(cur_file, sizeof(cur_file), "%s", s.file);
        update_text();
        /* keep the previous cover up until the new one is decoded: within
         * one album that's the same picture, so there is no flash */
        if(s.file[0]) art_request(s.file);
        else show_placeholder();
        last_dac_read_ms = 0;               /* re-read the DAC right away */
    }
    update_progress();
    update_transport_icon();
    update_format_line();
}

static void hide_controls(void)
{
    if(!controls_visible) return;
    controls_visible = false;
    lv_obj_fade_out(overlay, FADE_MS, 0);
    lv_timer_pause(idle_timer);
}

static void show_controls(void)
{
    lv_timer_reset(idle_timer);
    lv_timer_resume(idle_timer);
    if(controls_visible) return;
    controls_visible = true;
    lv_obj_fade_in(overlay, FADE_MS, 0);
}

static void idle_timer_cb(lv_timer_t * t)
{
    (void)t;
    hide_controls();
}

/* ------------------------------------------------------------- build */

static lv_obj_t * make_pill(lv_obj_t * parent, const char * text, const lv_font_t * font, int w, int h)
{
    lv_obj_t * o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, h / 2, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * l = lv_label_create(o);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_FG), 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return o;
}

static lv_obj_t * make_label(lv_obj_t * parent, const lv_font_t * font, uint32_t color, int x, int y, int w)
{
    lv_obj_t * l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(l, "");
    return l;
}

void nowplaying_create(void)
{
    art_init(UI_BASE);

    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* placeholder: token background with a big initial in an accent disc */
    placeholder = lv_obj_create(scr);
    lv_obj_set_size(placeholder, UI_BASE, UI_BASE);
    lv_obj_set_pos(placeholder, 0, 0);
    lv_obj_set_style_bg_color(placeholder, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(placeholder, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(placeholder, 0, 0);
    lv_obj_set_style_radius(placeholder, 0, 0);
    lv_obj_remove_flag(placeholder, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * disc = lv_obj_create(placeholder);
    lv_obj_set_size(disc, 220, 220);
    lv_obj_set_style_radius(disc, 110, 0);
    lv_obj_set_style_border_width(disc, 0, 0);
    lv_obj_set_style_bg_color(disc, lv_color_hex(UI_COLOR_FOCUS_BG), 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_align(disc, LV_ALIGN_CENTER, 0, -90);
    placeholder_letter = lv_label_create(disc);
    lv_obj_set_style_text_font(placeholder_letter, FONT_TITLE, 0);
    lv_obj_set_style_text_color(placeholder_letter, lv_color_hex(UI_COLOR_FOCUS_FG), 0);
    lv_label_set_text(placeholder_letter, "");
    lv_obj_center(placeholder_letter);

    art_img = lv_image_create(scr);
    lv_obj_add_flag(art_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(art_img);

    /* overlay strip: transparent at the top, near-black at the bottom */
    overlay = lv_obj_create(scr);
    lv_obj_set_size(overlay, UI_BASE, OVERLAY_H);
    lv_obj_set_pos(overlay, 0, UI_BASE - OVERLAY_H);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    static lv_grad_dsc_t grad;
    grad.dir = LV_GRAD_DIR_VER;
    /* two stops: lv_conf.h has LV_GRADIENT_MAX_STOPS 2; past frac 100 the
     * last stop's opacity holds to the bottom edge */
    grad.stops_count = 2;
    grad.stops[0].color = lv_color_black(); grad.stops[0].opa = LV_OPA_TRANSP; grad.stops[0].frac = 0;
    grad.stops[1].color = lv_color_black(); grad.stops[1].opa = 225;           grad.stops[1].frac = 100;
    lv_obj_set_style_bg_grad(overlay, &grad, 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);

    const int x0 = UI_MARGIN, w = UI_BASE - 2 * UI_MARGIN;
    title_lbl  = make_label(overlay, FONT_TITLE,     UI_COLOR_FG,  x0, 62,  w);
    artist_lbl = make_label(overlay, UI_FONT_LIST,   UI_COLOR_DIM, x0, 108, w);
    format_lbl = make_label(overlay, UI_FONT_HINT,   UI_COLOR_DIM, x0, 144, w);

    /* row 0: elapsed  [=====bar=====]  duration */
    const int row0_y = 184;
    elapsed_lbl = make_label(overlay, UI_FONT_HINT, UI_COLOR_FG, x0, row0_y - 4, 72);
    lv_label_set_text(elapsed_lbl, "0:00");
    duration_lbl = make_label(overlay, UI_FONT_HINT, UI_COLOR_FG, UI_BASE - UI_MARGIN - 72, row0_y - 4, 72);
    lv_obj_set_style_text_align(duration_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(duration_lbl, "--:--");
    bar = lv_bar_create(overlay);
    lv_obj_set_size(bar, w - 2 * 84, 8);
    lv_obj_set_pos(bar, x0 + 84, row0_y + 6);
    lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_bar_set_range(bar, 0, 1);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    knob = lv_obj_create(overlay);
    lv_obj_set_size(knob, 20, 20);
    lv_obj_set_style_radius(knob, 10, 0);
    lv_obj_set_style_border_width(knob, 0, 0);
    lv_obj_set_style_bg_color(knob, lv_color_hex(UI_COLOR_FOCUS_FG), 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    lv_obj_add_flag(knob, LV_OBJ_FLAG_HIDDEN);

    /* row 1: prev  play/pause  next */
    const int row1_y = 214, tr_w = 96, tr_h = 68, tr_gap = 40;
    static const char * const tr_sym[TR_COUNT] = { LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_NEXT };
    for(int i = 0; i < TR_COUNT; i++) {
        tr_btn[i] = make_pill(overlay, tr_sym[i], UI_FONT_LIST, tr_w, tr_h);
        lv_obj_set_pos(tr_btn[i], UI_BASE / 2 - (3 * tr_w + 2 * tr_gap) / 2 + i * (tr_w + tr_gap), row1_y);
        tr_lbl[i] = lv_obj_get_child(tr_btn[i], 0);
    }

    /* row 2: favorite  info  more */
    const int row2_y = 292, bt_h = 40;
    static const char * const bt_text[BT_COUNT] = { "Favorite", "Info", "More" };
    static const int bt_w[BT_COUNT] = { 150, 100, 110 };
    int total = 0;
    for(int i = 0; i < BT_COUNT; i++) total += bt_w[i];
    int gap = (w - total) / (BT_COUNT - 1), x = x0;
    for(int i = 0; i < BT_COUNT; i++) {
        bt_btn[i] = make_pill(overlay, bt_text[i], UI_FONT_HINT, bt_w[i], bt_h);
        lv_obj_set_pos(bt_btn[i], x, row2_y);
        x += bt_w[i] + gap;
    }

    apply_focus();
    update_text();

    status_timer = lv_timer_create(status_timer_cb, STATUS_POLL_MS, NULL);
    art_timer    = lv_timer_create(art_timer_cb, ART_POLL_MS, NULL);
    idle_timer   = lv_timer_create(idle_timer_cb, IDLE_HIDE_MS, NULL);
    lv_timer_pause(status_timer);
    lv_timer_pause(idle_timer);
    controls_visible = true;
}

/* ------------------------------------------------------------- public */

void nowplaying_show(void)
{
    active = true;
    row = ROW_TRANSPORT;
    tr_cur = TR_PLAY;
    apply_focus();
    lv_anim_delete(overlay, NULL);          /* cancel a fade still running */
    lv_obj_set_style_opa(overlay, LV_OPA_COVER, 0);
    controls_visible = true;
    lv_screen_load(scr);
    lv_timer_resume(status_timer);
    lv_timer_ready(status_timer);          /* refresh now, not in 500 ms */
    lv_timer_reset(idle_timer);
    lv_timer_resume(idle_timer);
}

void nowplaying_hide(void)
{
    active = false;
    lv_timer_pause(status_timer);
    lv_timer_pause(idle_timer);
}

static void transport_act(void)
{
    switch(tr_cur) {
        case TR_PREV: mpd_previous(); break;
        case TR_PLAY: mpd_toggle_pause(); break;
        case TR_NEXT: mpd_next(); break;
    }
    lv_timer_ready(status_timer);
}

np_result_t nowplaying_handle_action(ui_action_t a)
{
    if(!active) return NP_HANDLED;

    if(!controls_visible) {
        switch(a) {
            case ACT_BACK:  return NP_EXIT;
            case ACT_UP: case ACT_DOWN: case ACT_SELECT:
                show_controls();
                return NP_HANDLED;
            case ACT_LEFT: case ACT_RIGHT:
                /* reserved: page through the album's other images (phase B) */
                return NP_HANDLED;
            default:
                return NP_HANDLED;
        }
    }

    show_controls();                        /* any input restarts the idle clock */
    switch(a) {
        case ACT_UP:   if(row > 0) row--; apply_focus(); break;
        case ACT_DOWN: if(row < ROW_COUNT - 1) row++; apply_focus(); break;
        case ACT_LEFT:
        case ACT_RIGHT: {
            int dir = a == ACT_RIGHT ? 1 : -1;
            if(row == ROW_SCRUB) {
                mpd_seek_relative(dir * SEEK_STEP_S);
                lv_timer_ready(status_timer);
            }
            else if(row == ROW_TRANSPORT) { tr_cur = (tr_cur + dir + TR_COUNT) % TR_COUNT; apply_focus(); }
            else                          { bt_cur = (bt_cur + dir + BT_COUNT) % BT_COUNT; apply_focus(); }
            break;
        }
        case ACT_SELECT:
            if(row == ROW_TRANSPORT) transport_act();
            else if(row == ROW_SCRUB) { mpd_toggle_pause(); lv_timer_ready(status_timer); }
            /* ROW_BOTTOM: favorites / info / options arrive with curation */
            break;
        case ACT_BACK:
            hide_controls();
            break;
        case ACT_PLAYPAUSE: case ACT_NEXT: case ACT_PREV:
            lv_timer_ready(status_timer);   /* main.c already sent the command */
            break;
        default:
            break;
    }
    return NP_HANDLED;
}
