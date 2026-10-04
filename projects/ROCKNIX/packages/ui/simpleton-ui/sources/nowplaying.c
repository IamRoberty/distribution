/*
 * SimpletonOS UI - now-playing screen (implementation). See nowplaying.h.
 *
 * Layout (a shared screen on the square stage, measured in stage units
 * through PX() - layout.h; its own words come from the string table):
 * the art fills the square stage edge
 * to edge (album covers are square). On a TV the stage sits centred and the
 * space either side takes a muted colour drawn from the art (29 Sep 2026),
 * which fades across on a track change. A gradient strip over the bottom 340 px
 * carries title / artist / format line, then the three control rows:
 *
 *   row 0  scrub bar         left/right = seek +/-10 s
 *   row 1  transport         left/right moves between prev / play / next
 *   row 2  favorite/info/more (placeholders until curation lands)
 *
 * With the controls hidden, left/right pages through the album's images
 * (cover, back, booklet scans...). A wide page (a gatefold or booklet
 * spread) fills the screen top to bottom and left/right first pans across
 * it in equal stops about half a screen apart (a 2:1 gatefold: left page,
 * middle, right page - a scan that is 2.03:1 still takes exactly those
 * three); past its edge the press turns the page,
 * and going backwards lands on the previous page's right-hand end. A "2 / 11" badge in the top-right corner
 * shows with the controls whenever an album has more than one image, and
 * for a moment after each page turn.
 *
 * Status comes from MPD only when MPD says playback changed (its `idle`
 * notices, routed here by main.c as nowplaying_mpd_changed()), so a change
 * made from any other control surface (a phone app later) shows at once.
 * Between notices nothing is asked of MPD: the elapsed time is counted
 * forward locally from the last status, which is all MPD would report.
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
#include "strings.h"
#include "theme.h"

#include <ctype.h>
#include <glob.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CLOCK_MS         500     /* local progress-clock tick; no MPD traffic */
#define ART_POLL_MS      100
#define IDLE_HIDE_MS     4000
#define FADE_MS          250
#define SEEK_STEP_S      10.0f
#define BADGE_FLASH_MS   1500
#define PAN_STEP         (UI_BASE / 2)
#define PAN_MS           100

#define OVERLAY_H        PX(340)
#define FONT_TITLE       ui_font(36)
#define BG_FADE_MS       600

enum { ROW_SCRUB = 0, ROW_TRANSPORT, ROW_BOTTOM, ROW_COUNT };
enum { TR_PREV = 0, TR_PLAY, TR_NEXT, TR_COUNT };
enum { BT_FAV = 0, BT_INFO, BT_MORE, BT_COUNT };

static lv_obj_t * scr;
static lv_obj_t * stage;              /* the square everything is laid out in */
static lv_obj_t * art_img;
static lv_obj_t * placeholder;        /* plain fallback: only when the themed placeholder can't render */
static lv_obj_t * placeholder_letter;
static lv_obj_t * overlay;
static lv_obj_t * title_lbl, * artist_lbl, * format_lbl;
static lv_obj_t * bar, * knob, * elapsed_lbl, * duration_lbl;
static lv_obj_t * tr_btn[TR_COUNT], * tr_lbl[TR_COUNT];
static lv_obj_t * bt_btn[BT_COUNT];
static lv_obj_t * badge, * badge_lbl;

static lv_timer_t * clock_timer, * art_timer, * idle_timer, * badge_timer;

static bool active;
static bool controls_visible;
static int  row = ROW_TRANSPORT, tr_cur = TR_PLAY, bt_cur = BT_FAV;

static mpd_status_t st;
static bool     status_dirty = true;  /* MPD said something changed: fetch status */
static float    base_elapsed;         /* elapsed at the last fetch...            */
static uint32_t base_tick;            /* ...and when that was (lv_tick ms)       */

/* Fetch status on the next clock tick, which we make happen now. */
static void request_status(void);
static char cur_file[1024];           /* file the art on screen belongs to */
static lv_image_dsc_t art_dsc[2];     /* alternate so LVGL sees a new src */
static int art_slot;
static int art_index, art_count = 1;
static bool badge_shown;

static void set_side_colour(uint32_t colour);
static int  pan_x, pan_max;           /* wide page: current offset, how far it goes */
static int  pan_i, pan_n;             /* which stop of how many (0 = left edge) */
static bool enter_at_end;             /* paged backwards: show the new page's right end */
static bool page_pending;             /* a page turn is on its way from the art worker */
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
    if(*bits == 'f') ui_strf(out, len, S_FORMAT_FLOAT, "rate", rate_s, NULL);
    else {
        char bits_s[16];
        snprintf(bits_s, sizeof(bits_s), "%d", atoi(bits));
        ui_strf(out, len, S_FORMAT_PCM, "rate", rate_s, "bits", bits_s, NULL);
    }
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
    char line[256], name[64], fmt[32] = "";
    snprintf(name, sizeof(name), "%s", T(S_DAC_UNNAMED));
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
        char mult[16];
        snprintf(mult, sizeof(mult), "%d", (int)lround(freq * bits / 44100.0));
        ui_strf(out, len, S_DAC_DSD_NATIVE, "dac", name, "n", mult, NULL);
    }
    else if(dop) {
        ui_strf(out, len, S_DAC_DOP, "dac", name, "rate", nearest_rate(freq), NULL);
    }
    else if(fmt[0]) {
        ui_strf(out, len, S_DAC_PCM, "dac", name, "rate", nearest_rate(freq), NULL);
    }
    else {
        ui_strf(out, len, S_DAC_RUNNING, "dac", name, NULL);
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
    else            snprintf(text, sizeof(text), "%s %s   " LV_SYMBOL_RIGHT "   %s", codec, src, T(S_DAC_IDLE));
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
    lv_obj_set_pos(knob, lv_obj_get_x(bar) + x - PX(10), lv_obj_get_y(bar) + lv_obj_get_height(bar) / 2 - PX(10));
}

static void update_transport_icon(void)
{
    lv_label_set_text(tr_lbl[TR_PLAY], strcmp(st.state, "play") == 0 ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

static void update_text(void)
{
    if(!st.file[0]) {
        lv_label_set_text(title_lbl, T(S_NP_NOTHING));
        lv_label_set_text(artist_lbl, T(S_NP_PICK_A_TRACK));
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
    pan_x = pan_max = pan_i = pan_n = 0;
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
    set_side_colour(UI_COLOR_BG);
}

/* ------------------------------------------------------ side colour */

/*
 * The colour beside the stage on a TV (and behind art that isn't square).
 * Dominant colour of the picture - the fullest bin of a coarse 4-bit-per-
 * channel histogram, saturated pixels counting a little extra so a small
 * vivid subject can beat a big grey field - averaged within that bin, then
 * muted and darkened so it sits behind the cover rather than competing.
 * The three constants are the knobs to tune by eye.
 */
#define SIDE_SAT_KEEP   0.70f   /* fraction of the art colour's saturation kept */
#define SIDE_V_MAX      0.32f   /* never brighter than this (HSV value, 0..1)   */
#define SIDE_V_MIN      0.08f   /* never darker - pure black reads as "off"     */

static uint32_t side_now = 0x000000;

static uint32_t side_colour_from(const uint8_t * px, int w, int h)
{
    static uint32_t weight[4096], samples[4096], sum[4096][3];
    memset(weight, 0, sizeof(weight));
    memset(samples, 0, sizeof(samples));
    memset(sum, 0, sizeof(sum));
    int step = (w > h ? w : h) / 96;            /* ~100 x 100 samples whatever the size */
    if(step < 1) step = 1;
    for(int y = 0; y < h; y += step) {
        const uint8_t * row = px + (size_t)y * w * 4;
        for(int x = 0; x < w; x += step) {
            int b = row[x * 4], g = row[x * 4 + 1], rr = row[x * 4 + 2];   /* XRGB8888, little endian */
            int mx = rr > g ? (rr > b ? rr : b) : (g > b ? g : b);
            int mn = rr < g ? (rr < b ? rr : b) : (g < b ? g : b);
            int bin = ((rr >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
            weight[bin] += 2 + (mx ? 4 * (mx - mn) / mx : 0);           /* 2 (grey) .. 6 (vivid) */
            samples[bin]++;
            sum[bin][0] += (uint32_t)rr; sum[bin][1] += (uint32_t)g; sum[bin][2] += (uint32_t)b;
        }
    }
    int best = 0;
    for(int i = 1; i < 4096; i++) if(weight[i] > weight[best]) best = i;
    uint32_t n = samples[best];
    if(!n) return UI_COLOR_BG;
    float r = sum[best][0] / (255.0f * n), g = sum[best][1] / (255.0f * n), b = sum[best][2] / (255.0f * n);

    /* RGB -> HSV, mute, back */
    float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    float hue = 0;
    if(d > 0) {
        if(mx == r)      hue = fmodf((g - b) / d, 6.0f);
        else if(mx == g) hue = (b - r) / d + 2.0f;
        else             hue = (r - g) / d + 4.0f;
        if(hue < 0) hue += 6.0f;
    }
    float sat = mx > 0 ? d / mx : 0, val = mx;
    sat *= SIDE_SAT_KEEP;
    val = fminf(fmaxf(val, SIDE_V_MIN), SIDE_V_MAX);
    float c = val * sat, xx = c * (1 - fabsf(fmodf(hue, 2.0f) - 1)), m = val - c;
    float o[3];
    switch((int)hue) {
        case 0:  o[0] = c;  o[1] = xx; o[2] = 0;  break;
        case 1:  o[0] = xx; o[1] = c;  o[2] = 0;  break;
        case 2:  o[0] = 0;  o[1] = c;  o[2] = xx; break;
        case 3:  o[0] = 0;  o[1] = xx; o[2] = c;  break;
        case 4:  o[0] = xx; o[1] = 0;  o[2] = c;  break;
        default: o[0] = c;  o[1] = 0;  o[2] = xx; break;
    }
    uint32_t R = (uint32_t)lroundf((o[0] + m) * 255), G = (uint32_t)lroundf((o[1] + m) * 255), B = (uint32_t)lroundf((o[2] + m) * 255);
    return (R << 16) | (G << 8) | B;
}

static uint32_t side_from, side_to;

static void side_fade_exec(void * obj, int32_t v)
{
    lv_color_t c = lv_color_mix(lv_color_hex(side_to), lv_color_hex(side_from), (uint8_t)v);
    lv_obj_set_style_bg_color(obj, c, 0);
    side_now = lv_color_to_u32(c) & 0xFFFFFF;
}

static void set_side_colour(uint32_t colour)
{
    if(colour == side_to && lv_anim_get(scr, side_fade_exec)) return;
    lv_anim_delete(scr, side_fade_exec);
    side_from = side_now;
    side_to = colour;
    if(side_from == side_to) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, scr);
    lv_anim_set_exec_cb(&a, side_fade_exec);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, BG_FADE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

/* ------------------------------------------------------- page badge */

static void badge_set(bool on)
{
    if(on == badge_shown) return;
    badge_shown = on;
    lv_anim_delete(badge, NULL);
    if(on) lv_obj_fade_in(badge, FADE_MS, 0);
    else   lv_obj_fade_out(badge, FADE_MS, 0);
}

static void badge_refresh(void)
{
    if(art_count > 1) lv_label_set_text_fmt(badge_lbl, "%d / %d", art_index + 1, art_count);
    bool flashing = !lv_timer_get_paused(badge_timer);
    badge_set(art_count > 1 && (controls_visible || flashing));
}

static void badge_flash(void)
{
    lv_timer_reset(badge_timer);
    lv_timer_resume(badge_timer);
    badge_refresh();
}

static void badge_timer_cb(lv_timer_t * t)
{
    lv_timer_pause(t);
    badge_refresh();
}

/* keep_pan: same wide page re-sent after a track change - stay where the
 * reader was instead of jumping back to its left edge */
static void set_art(const art_result_t * r, bool keep_pan)
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
    lv_anim_delete(art_img, NULL);
    pan_max = r->w > UI_BASE ? r->w - UI_BASE : 0;
    pan_n = pan_max ? (int)lround((double)pan_max / PAN_STEP) : 0;
    if(pan_max && pan_n < 1) pan_n = 1;
    if(!keep_pan || pan_i > pan_n) pan_i = enter_at_end ? pan_n : 0;
    pan_x = pan_n ? pan_i * pan_max / pan_n : 0;
    enter_at_end = false;
    page_pending = false;
    /* lv_obj_center() leaves the object centre-aligned, and a later
     * set_pos() is then an offset from the centre - so a wide page has to
     * be put back on top-left alignment before its x means "left edge" */
    if(pan_max) { lv_obj_set_align(art_img, LV_ALIGN_TOP_LEFT); lv_obj_set_pos(art_img, -pan_x, (UI_BASE - r->h) / 2); }
    else        lv_obj_center(art_img);
    lv_obj_remove_flag(art_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(placeholder, LV_OBJ_FLAG_HIDDEN);

    /* the previous slot is no longer referenced by the image object */
    free(old_pixels);
}

static void pan_exec(void * obj, int32_t x) { lv_obj_set_x(obj, x); }

static void pan_to(int i)
{
    pan_i = i;
    int x = pan_i * pan_max / pan_n;
    pan_x = x;
    lv_anim_delete(art_img, pan_exec);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, art_img);
    lv_anim_set_exec_cb(&a, pan_exec);
    lv_anim_set_values(&a, lv_obj_get_x(art_img), -x);
    lv_anim_set_duration(&a, PAN_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);
}

/* Left/right with the controls hidden: pan a wide page, else turn the page. */
static void browse_art(int dir)
{
    if(dir > 0 && pan_i < pan_n) { pan_to(pan_i + 1); return; }
    if(dir < 0 && pan_i > 0)     { pan_to(pan_i - 1); return; }
    if(art_count <= 1) return;
    art_index = (art_index + dir + art_count) % art_count;
    enter_at_end = dir < 0;
    page_pending = true;
    art_request_page(art_index);
    badge_flash();                          /* counter moves now, image follows */
}

/* ------------------------------------------------------------- timers */

static void art_timer_cb(lv_timer_t * t)
{
    (void)t;
    bool found;
    art_result_t r;
    if(!art_poll(&found, &r)) return;
    if(found) {
        bool keep_pan = !page_pending && pan_max > 0 && r.index == art_index && r.w - UI_BASE == pan_max;
        art_index = r.index;
        art_count = r.count > 0 ? r.count : 1;
        set_side_colour(side_colour_from(r.pixels, r.w, r.h));
        set_art(&r, keep_pan);
    }
    else      { art_index = 0; art_count = 1; show_placeholder(); }
    badge_refresh();
}

static void refresh_status(void)
{
    mpd_status_t s;
    bool ok = mpd_status(&s);
    if(!ok) { memset(&s, 0, sizeof(s)); s.songid = -1; }
    bool track_changed = strcmp(s.file, cur_file) != 0;
    st = s;
    base_elapsed = s.elapsed;
    base_tick = lv_tick_get();
    status_dirty = false;

    if(track_changed) {
        snprintf(cur_file, sizeof(cur_file), "%s", s.file);
        update_text();
        /* keep the previous cover up until the new one is decoded: within
         * one album that's the same picture, so there is no flash */
        enter_at_end = page_pending = false;
        if(s.file[0]) art_request(s.file, s.artist, s.album, s.title);
        else { art_index = 0; art_count = 1; show_placeholder(); badge_refresh(); }
        last_dac_read_ms = 0;               /* re-read the DAC right away */
    }
    update_progress();
    update_transport_icon();
    update_format_line();
}

static void clock_timer_cb(lv_timer_t * t)
{
    (void)t;
    if(status_dirty) { refresh_status(); return; }
    if(strcmp(st.state, "play") == 0) {
        float e = base_elapsed + (float)(lv_tick_get() - base_tick) / 1000.0f;
        st.elapsed = (st.duration > 0 && e > st.duration) ? st.duration : e;
    }
    update_progress();
    update_format_line();                   /* re-reads the DAC file every 1.5 s, local */
}

static void request_status(void)
{
    status_dirty = true;
    if(active) lv_timer_ready(clock_timer);
}

static void hide_controls(void)
{
    if(!controls_visible) return;
    controls_visible = false;
    lv_obj_fade_out(overlay, FADE_MS, 0);
    lv_timer_pause(idle_timer);
    badge_refresh();
}

static void show_controls(void)
{
    lv_timer_reset(idle_timer);
    lv_timer_resume(idle_timer);
    if(controls_visible) return;
    controls_visible = true;
    lv_obj_fade_in(overlay, FADE_MS, 0);
    badge_refresh();
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
    stage = ui_stage_create(scr);

    /* placeholder: token background with a big initial in an accent disc */
    placeholder = lv_obj_create(stage);
    lv_obj_set_size(placeholder, UI_BASE, UI_BASE);
    lv_obj_set_pos(placeholder, 0, 0);
    lv_obj_set_style_bg_color(placeholder, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(placeholder, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(placeholder, 0, 0);
    lv_obj_set_style_radius(placeholder, 0, 0);
    lv_obj_remove_flag(placeholder, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * disc = lv_obj_create(placeholder);
    lv_obj_set_size(disc, PX(220), PX(220));
    lv_obj_set_style_radius(disc, PX(110), 0);
    lv_obj_set_style_border_width(disc, 0, 0);
    lv_obj_set_style_bg_color(disc, lv_color_hex(UI_COLOR_FOCUS_BG), 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_align(disc, LV_ALIGN_CENTER, 0, PX(-90));
    placeholder_letter = lv_label_create(disc);
    lv_obj_set_style_text_font(placeholder_letter, FONT_TITLE, 0);
    lv_obj_set_style_text_color(placeholder_letter, lv_color_hex(UI_COLOR_FOCUS_FG), 0);
    lv_label_set_text(placeholder_letter, "");
    lv_obj_center(placeholder_letter);

    art_img = lv_image_create(stage);
    lv_obj_add_flag(art_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(art_img);

    /* overlay strip: transparent at the top, near-black at the bottom */
    overlay = lv_obj_create(stage);
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
    title_lbl  = make_label(overlay, FONT_TITLE,     UI_COLOR_FG,  x0, PX(62),  w);
    artist_lbl = make_label(overlay, UI_FONT_LIST,   UI_COLOR_DIM, x0, PX(108), w);
    format_lbl = make_label(overlay, UI_FONT_HINT,   UI_COLOR_DIM, x0, PX(144), w);

    /* row 0: elapsed  [=====bar=====]  duration */
    const int row0_y = PX(184), time_w = PX(72);
    elapsed_lbl = make_label(overlay, UI_FONT_HINT, UI_COLOR_FG, x0, row0_y - PX(4), time_w);
    lv_label_set_text(elapsed_lbl, "0:00");
    duration_lbl = make_label(overlay, UI_FONT_HINT, UI_COLOR_FG, UI_BASE - UI_MARGIN - time_w, row0_y - PX(4), time_w);
    lv_obj_set_style_text_align(duration_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(duration_lbl, "--:--");
    bar = lv_bar_create(overlay);
    lv_obj_set_size(bar, w - 2 * PX(84), PX(8));
    lv_obj_set_pos(bar, x0 + PX(84), row0_y + PX(6));
    lv_obj_set_style_radius(bar, PX(4), LV_PART_MAIN);
    lv_obj_set_style_radius(bar, PX(4), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_bar_set_range(bar, 0, 1);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    knob = lv_obj_create(overlay);
    lv_obj_set_size(knob, PX(20), PX(20));
    lv_obj_set_style_radius(knob, PX(10), 0);
    lv_obj_set_style_border_width(knob, 0, 0);
    lv_obj_set_style_bg_color(knob, lv_color_hex(UI_COLOR_FOCUS_FG), 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    lv_obj_add_flag(knob, LV_OBJ_FLAG_HIDDEN);

    /* row 1: prev  play/pause  next */
    const int row1_y = PX(214), tr_w = PX(96), tr_h = PX(68), tr_gap = PX(40);
    static const char * const tr_sym[TR_COUNT] = { LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_NEXT };
    for(int i = 0; i < TR_COUNT; i++) {
        tr_btn[i] = make_pill(overlay, tr_sym[i], UI_FONT_LIST, tr_w, tr_h);
        lv_obj_set_pos(tr_btn[i], UI_BASE / 2 - (3 * tr_w + 2 * tr_gap) / 2 + i * (tr_w + tr_gap), row1_y);
        tr_lbl[i] = lv_obj_get_child(tr_btn[i], 0);
    }

    /* row 2: favorite  info  more */
    const int row2_y = PX(292), bt_h = PX(40);
    const char * const bt_text[BT_COUNT] = { T(S_NP_FAVORITE), T(S_NP_INFO), T(S_NP_MORE) };
    int bt_w[BT_COUNT] = { PX(150), PX(100), PX(110) };
    int total = 0;
    for(int i = 0; i < BT_COUNT; i++) total += bt_w[i];
    int gap = (w - total) / (BT_COUNT - 1), x = x0;
    for(int i = 0; i < BT_COUNT; i++) {
        bt_btn[i] = make_pill(overlay, bt_text[i], UI_FONT_HINT, bt_w[i], bt_h);
        lv_obj_set_pos(bt_btn[i], x, row2_y);
        x += bt_w[i] + gap;
    }

    /* page badge, top-right over the art: "2 / 11" */
    badge = lv_obj_create(stage);
    lv_obj_set_height(badge, PX(40));
    lv_obj_set_width(badge, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(badge, PX(16), 0);
    lv_obj_set_style_pad_ver(badge, 0, 0);
    lv_obj_set_style_radius(badge, PX(20), 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_set_style_bg_color(badge, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(badge, 170, 0);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -UI_MARGIN, UI_MARGIN);
    lv_obj_set_style_opa(badge, LV_OPA_TRANSP, 0);
    badge_lbl = lv_label_create(badge);
    lv_obj_set_style_text_font(badge_lbl, UI_FONT_HINT, 0);
    lv_obj_set_style_text_color(badge_lbl, lv_color_hex(UI_COLOR_FG), 0);
    lv_label_set_text(badge_lbl, "");
    lv_obj_center(badge_lbl);

    apply_focus();
    update_text();

    clock_timer  = lv_timer_create(clock_timer_cb, CLOCK_MS, NULL);
    art_timer    = lv_timer_create(art_timer_cb, ART_POLL_MS, NULL);
    idle_timer   = lv_timer_create(idle_timer_cb, IDLE_HIDE_MS, NULL);
    badge_timer  = lv_timer_create(badge_timer_cb, BADGE_FLASH_MS, NULL);
    lv_timer_pause(clock_timer);
    lv_timer_pause(idle_timer);
    lv_timer_pause(badge_timer);
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
    status_dirty = true;                    /* nothing was watched while hidden */
    lv_timer_resume(clock_timer);
    lv_timer_ready(clock_timer);            /* fetch now, not in 500 ms */
    lv_timer_reset(idle_timer);
    lv_timer_resume(idle_timer);
    badge_refresh();
}

void nowplaying_mpd_changed(void)
{
    request_status();                       /* while hidden: fetched on show */
}

void nowplaying_hide(void)
{
    active = false;
    lv_timer_pause(clock_timer);
    lv_timer_pause(idle_timer);
}

static void transport_act(void)
{
    switch(tr_cur) {
        case TR_PREV: mpd_previous(); break;
        case TR_PLAY: mpd_toggle_pause(); break;
        case TR_NEXT: mpd_next(); break;
    }
    request_status();
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
                browse_art(a == ACT_RIGHT ? 1 : -1);
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
                request_status();
            }
            else if(row == ROW_TRANSPORT) { tr_cur = (tr_cur + dir + TR_COUNT) % TR_COUNT; apply_focus(); }
            else                          { bt_cur = (bt_cur + dir + BT_COUNT) % BT_COUNT; apply_focus(); }
            break;
        }
        case ACT_SELECT:
            if(row == ROW_TRANSPORT) transport_act();
            else if(row == ROW_SCRUB) { mpd_toggle_pause(); request_status(); }
            /* ROW_BOTTOM: favorites / info / options arrive with curation */
            break;
        case ACT_BACK:
            hide_controls();
            break;
        case ACT_PLAYPAUSE: case ACT_NEXT: case ACT_PREV:
            request_status();               /* main.c already sent the command */
            break;
        default:
            break;
    }
    return NP_HANDLED;
}
