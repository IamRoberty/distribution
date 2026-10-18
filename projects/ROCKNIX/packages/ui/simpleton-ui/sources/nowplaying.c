/*
 * SimpletonOS UI - now-playing screen (implementation). See nowplaying.h.
 *
 * 0.23 (16 Oct 2026, Ian's overhaul): the page is built from the cover.
 * The art fills the square stage edge to edge; a gradient strip over the
 * bottom 420 px takes the cover's dark colour (palette.h's roles, pushed
 * darker so the words read), solid behind the text, gone above the title.
 * On a TV the stage sits centred and the space either side takes the same
 * colour, darker still. The strip carries, one line each and trimmed with
 * "...": the title, the artist, the format line (what MPD decoded and what
 * the DAC gets - the truth from both sides, see parse_stream0); then three
 * focusable rows of pictures, no words and no bubbles:
 *
 *   row 0  the play bar     left/right = seek +/-10 s; selected: the bar
 *                           and its knob take the accent, the knob grows
 *   row 1  prev play next   drawn icons; the selected one grows ~35 % and
 *                           takes the cover's accent (npicons.h)
 *   row 2  heart  mode  list  gear   at the quarter points:
 *            heart  red and filled when the track is a favourite, an
 *                   outline otherwise; A toggles
 *            mode   play through -> repeat album -> repeat track -> random,
 *                   the icon shows the current one; A cycles
 *            list   the queue (the album, mixtape or folder playing) as a
 *                   panel over the art, in the same colours; A plays a
 *                   track, Back closes
 *            gear   the track's menu (favourite, mixtapes, collections)
 *
 * The accent is the cover's most vivid colour that reads on the strip,
 * else one of the theme's palette colours that does (tilestyle_page_accents);
 * the ink is cream or the theme's dark ink, whichever reads better.
 *
 * With the controls hidden, left/right pages through the album's images
 * (cover, back, booklet scans...). A wide page (a gatefold or booklet
 * spread) fills the screen top to bottom and left/right first pans across
 * it in equal stops about half a screen apart (a 2:1 gatefold: left page,
 * middle, right page); past its edge the press turns the page, and going
 * backwards lands on the previous page's right-hand end. A "2 / 11" badge
 * in the top-right corner shows with the controls whenever an album has
 * more than one image, and for a moment after each page turn.
 *
 * Hosted in the album page (0.17b, cut down in 0.23): the stage shows that
 * album's own cover, pages and all, and never its controls, and never
 * follows playback - the page's picture is the page's.
 *
 * Status comes from MPD only when MPD says playback changed (its `idle`
 * notices, routed here by main.c as nowplaying_mpd_changed()), so a change
 * made from any other control surface (a phone app later) shows at once.
 * Between notices nothing is asked of MPD: the elapsed time is counted
 * forward locally from the last status, which is all MPD would report.
 */
#include "nowplaying.h"
#include "power.h"
#include "art.h"
#include "mpdc.h"
#include "curation.h"
#include "objmenu.h"
#include "npicons.h"
#include "palette.h"
#include "strings.h"
#include "theme.h"
#include "tilestyle.h"
#include "src/misc/cache/instance/lv_image_cache.h"   /* lv_image_cache_drop, not in lvgl.h */

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

#define OVERLAY_H        PX(420)
#define GRAD_SOLID_FRAC  90      /* of 255: the strip is solid from 35 % of its height down */
#define FONT_TITLE       ui_font(36)
#define FONT_ARTIST      ui_font(26)
#define BG_FADE_MS       600
#define FAV_RED          0xE0242C  /* Simpleton's favourite colour (albumpage.c) */

/* the rows, in stage units from the strip's top */
#define Y_TITLE          PX(150)
#define Y_ARTIST         PX(196)
#define Y_FORMAT         PX(232)
#define Y_BAR            PX(274)   /* the bar's centre line */
#define Y_TRANSPORT      PX(322)   /* icon centres */
#define Y_BOTTOM         PX(388)
#define ICON_TRANSPORT   PX(44)
#define ICON_BOTTOM      PX(30)
#define ICON_GROW        135       /* percent, the selected one */
#define KNOB_R           PX(8)
#define KNOB_R_SEL       PX(12)

enum { ROW_SCRUB = 0, ROW_TRANSPORT, ROW_BOTTOM, ROW_COUNT };
enum { TR_PREV = 0, TR_PLAY, TR_NEXT, TR_COUNT };
enum { BT_FAV = 0, BT_MODE, BT_LIST, BT_MORE, BT_COUNT };
typedef enum { MODE_THROUGH = 0, MODE_REPEAT, MODE_REPEAT_ONE, MODE_RANDOM, MODE_COUNT } play_mode_t;

static lv_obj_t * scr;
static lv_obj_t * stage;              /* the square everything is laid out in */
static lv_obj_t * art_img;
static lv_obj_t * placeholder;        /* plain fallback: only when the themed placeholder can't render */
static lv_obj_t * placeholder_letter;
static lv_obj_t * overlay;
static lv_obj_t * title_lbl, * artist_lbl, * format_lbl;
static lv_obj_t * bar, * knob, * elapsed_lbl, * duration_lbl;
static lv_obj_t * tr_img[TR_COUNT];
static lv_obj_t * bt_img[BT_COUNT];
static lv_obj_t * badge, * badge_lbl;
static lv_grad_dsc_t grad;            /* the strip's gradient: recoloured per cover */

/* the colours of the moment (the cover's, or the plain ones) */
static uint32_t c_bg = UI_COLOR_BG, c_ink = UI_COLOR_FG, c_dim = UI_COLOR_DIM, c_sel = UI_COLOR_FOCUS_BG, c_text = UI_COLOR_FG;

static lv_timer_t * clock_timer, * art_timer, * idle_timer, * badge_timer;

static bool active;
static bool controls_visible;
static bool mpd_away;                 /* MPD is restarting for a new DAC (0.18): say so */
static bool embedded;                 /* the stage lives in the album page's screen (0.17b) */
static bool host_hides_art;           /* ...which keeps its own picture: ours stays hidden */
static bool host_colours;             /* the strip wears the host page's colours (0.23e) */
static int  art_w, art_h;             /* the art on screen, for recolouring after a hosting */
static bool badge_single;             /* the badge may show "1 / 1" while it flashes (the page's cover browse) */
static char art_shown_file[1024];     /* the file the art on screen was decoded for */
static char expected_file[1024];      /* the file whose art should be up (hosted: the page's preview) */
static bool art_is_placeholder;       /* the art on screen is the themed placeholder, not a cover (11 Oct) */
static int  row = ROW_TRANSPORT, tr_cur = TR_PLAY, bt_cur = BT_FAV;
static void (*fav_cb)(void);
void nowplaying_set_curation_callback(void (*on_changed)(void)) { fav_cb = on_changed; }

static mpd_status_t st;
static bool     status_dirty = true;  /* MPD said something changed: fetch status */
static float    base_elapsed;         /* elapsed at the last fetch...            */
static uint32_t base_tick;            /* ...and when that was (lv_tick ms)       */

/* Fetch status on the next clock tick, which we make happen now. */
static void request_status(void);
static char cur_file[1024];           /* the current song as the last status said */
static char req_file[1024];           /* the file the last art request was for (a preview's, or the song's) */
static lv_image_dsc_t art_dsc[2];     /* alternate so LVGL sees a new src */
static int art_slot;
static int art_index, art_count = 1;
static bool badge_shown;
static bool badge_held;               /* art browse (0.24): the badge stays while it lasts */

static void set_side_colour(uint32_t colour);
static void apply_host_art(void);
static void restore_playing_art(void);
static void apply_focus(void);
static void panel_close(void);
static void colours_set_bg(uint32_t bg, uint32_t sel_hint);
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
    char text[400];
    if(mpd_away || !mpd_is_connected()) snprintf(text, sizeof(text), "%s %s   " LV_SYMBOL_RIGHT "   %s", codec, src, T(S_NP_SWITCHING_DAC));
    else if(dac_line[0]) snprintf(text, sizeof(text), "%s %s   " LV_SYMBOL_RIGHT "   %s", codec, src, dac_line);
    else if(st.error[0]) {
        /* MPD could not open its output (11 Oct: "Failed to open "DAC"
         * (alsa); Failed to open ALSA device "hw:CARD=AUDIO,DEV=0": No such
         * device") - the device part, so the screen says which DAC MPD is
         * trying, instead of "DAC idle" */
        const char * dev = strstr(st.error, "hw:");
        char what[160];
        if(dev) { snprintf(what, sizeof(what), "%s", dev); what[strcspn(what, "\"")] = 0; }
        else snprintf(what, sizeof(what), "%.150s", st.error);
        char msg[200];
        ui_strf(msg, sizeof(msg), S_DAC_ERROR, "error", what, NULL);
        snprintf(text, sizeof(text), "%s %s   " LV_SYMBOL_RIGHT "   %s", codec, src, msg);
    }
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

static play_mode_t current_mode(void)
{
    if(st.single) return MODE_REPEAT_ONE;
    if(st.random) return MODE_RANDOM;
    if(st.repeat) return MODE_REPEAT;
    return MODE_THROUGH;
}

static npicon_t mode_icon(play_mode_t m)
{
    switch(m) {
        case MODE_REPEAT:     return NPI_REPEAT;
        case MODE_REPEAT_ONE: return NPI_REPEAT_ONE;
        case MODE_RANDOM:     return NPI_SHUFFLE;
        default:              return NPI_THROUGH;
    }
}

/* An icon object: the picture at `size` in `colour`, centred on (cx, cy). */
static void set_icon(lv_obj_t * img, npicon_t kind, int size, uint32_t colour, int cx, int cy)
{
    const lv_image_dsc_t * d = npicon_dsc(kind, size, colour);
    if(!d) return;
    lv_image_set_src(img, d);
    lv_obj_set_size(img, size, size);
    lv_obj_set_pos(img, cx - size / 2, cy - size / 2);
}

static int icon_x_transport(int i) { return UI_BASE * (32 + 18 * i) / 100; }   /* 32 %, 50 %, 68 % */
static int icon_x_bottom(int i)    { return UI_BASE * (1 + 2 * i) / 8; }      /* the quarter points */

static void apply_focus(void)
{
    bool scrub = row == ROW_SCRUB;
    lv_obj_set_style_bg_color(bar, lv_color_hex(scrub ? c_sel : c_ink), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(c_dim), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_60, LV_PART_MAIN);
    int kr = scrub ? KNOB_R_SEL : KNOB_R;
    lv_obj_set_size(knob, 2 * kr, 2 * kr);
    lv_obj_set_style_radius(knob, kr, 0);
    lv_obj_set_style_bg_color(knob, lv_color_hex(scrub ? c_sel : c_ink), 0);

    static const npicon_t tr_kind[TR_COUNT] = { NPI_PREV, NPI_PLAY, NPI_NEXT };
    for(int i = 0; i < TR_COUNT; i++) {
        bool f = row == ROW_TRANSPORT && i == tr_cur;
        npicon_t k = tr_kind[i];
        if(i == TR_PLAY && strcmp(st.state, "play") == 0) k = NPI_PAUSE;
        int size = f ? ICON_TRANSPORT * ICON_GROW / 100 : ICON_TRANSPORT;
        set_icon(tr_img[i], k, size, f ? c_sel : c_ink, icon_x_transport(i), Y_TRANSPORT);
    }
    bool fav = st.file[0] && curation_is_favorite(st.file);
    for(int i = 0; i < BT_COUNT; i++) {
        bool f = row == ROW_BOTTOM && i == bt_cur;
        npicon_t k;
        uint32_t col = f ? c_sel : c_ink;
        switch(i) {
            case BT_FAV:  k = fav ? NPI_HEART_FILLED : NPI_HEART; if(fav) col = FAV_RED; break;
            case BT_MODE: k = mode_icon(current_mode()); break;
            case BT_LIST: k = NPI_LIST; break;
            default:      k = NPI_GEAR; break;
        }
        int size = f ? ICON_BOTTOM * ICON_GROW / 100 : ICON_BOTTOM;
        set_icon(bt_img[i], k, size, col, icon_x_bottom(i), Y_BOTTOM);
    }
}

static void place_knob(void)
{
    int32_t w = lv_obj_get_width(bar);
    int32_t range = lv_bar_get_max_value(bar);
    int32_t x = range > 0 ? (int32_t)((int64_t)lv_bar_get_value(bar) * w / range) : 0;
    int kr = lv_obj_get_width(knob) / 2;
    lv_obj_set_pos(knob, lv_obj_get_x(bar) + x - kr, lv_obj_get_y(bar) + lv_obj_get_height(bar) / 2 - kr);
}

void nowplaying_curation_changed(void) { if(overlay) apply_focus(); }

/* The strip, the words and the icons in the colours of the moment. */
static void apply_colours(void)
{
    if(active && !embedded) ui_colours_set(c_bg, c_ink, c_sel);   /* a menu over this page wears its colours (0.25a) */
    grad.stops[0].color = lv_color_hex(c_bg);
    grad.stops[1].color = lv_color_hex(c_bg);
    lv_obj_set_style_bg_grad(overlay, &grad, 0);
    lv_obj_invalidate(overlay);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(c_ink), 0);
    lv_obj_set_style_text_color(artist_lbl, lv_color_hex(c_dim), 0);
    lv_obj_set_style_text_color(format_lbl, lv_color_hex(c_dim), 0);
    lv_obj_set_style_text_color(elapsed_lbl, lv_color_hex(c_ink), 0);
    lv_obj_set_style_text_color(duration_lbl, lv_color_hex(c_ink), 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(c_bg), 0);
    lv_obj_set_style_text_color(badge_lbl, lv_color_hex(c_ink), 0);
    apply_focus();
    place_knob();
}

/* RRGGBB mixed towards black: f of the colour kept (0..1) */
static uint32_t darken(uint32_t c, float f)
{
    unsigned r = (unsigned)lroundf(((c >> 16) & 0xFF) * f), g = (unsigned)lroundf(((c >> 8) & 0xFF) * f), b = (unsigned)lroundf((c & 0xFF) * f);
    return (r << 16) | (g << 8) | b;
}

static uint32_t mix(uint32_t a, uint32_t b, int pct_a)
{
    unsigned r = (((a >> 16) & 0xFF) * pct_a + ((b >> 16) & 0xFF) * (100 - pct_a)) / 100;
    unsigned g = (((a >> 8) & 0xFF) * pct_a + ((b >> 8) & 0xFF) * (100 - pct_a)) / 100;
    unsigned bl = ((a & 0xFF) * pct_a + (b & 0xFF) * (100 - pct_a)) / 100;
    return (r << 16) | (g << 8) | bl;
}

static float lightness(uint32_t c)
{
    return (0.2126f * ((c >> 16) & 0xFF) + 0.7152f * ((c >> 8) & 0xFF) + 0.0722f * (c & 0xFF)) / 255.0f;
}

/* The page's colours from the cover just decoded: the strip takes the
 * palette's background role pushed down to a dark shade (the words have to
 * read over it, whatever the cover), the ink is whichever of cream and the
 * theme's dark ink reads better on that, the accent the cover's most vivid
 * colour that reads on it - else a theme palette colour that does. */
/* Brown and skin: a hue from orange to yellow-brown with middling
 * saturation and lightness. Ian, 17 Oct: a brown strip under a tape looks
 * bad, and skin tones pulled from a cover aren't the colours one would
 * pick to go with it - "it's usually the stuff around the person". */
static bool skin_or_brown(uint32_t c)
{
    float r = ((c >> 16) & 0xFF) / 255.0f, g = ((c >> 8) & 0xFF) / 255.0f, b = (c & 0xFF) / 255.0f;
    float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    if(d < 0.04f) return false;                            /* grey */
    float hue = 0;
    if(mx == r)      hue = fmodf((g - b) / d, 6.0f);
    else if(mx == g) hue = (b - r) / d + 2.0f;
    else             hue = (r - g) / d + 4.0f;
    if(hue < 0) hue += 6.0f;
    hue *= 60;
    float sat = mx > 0 ? d / mx : 0;
    return hue >= 8 && hue <= 50 && sat >= 0.18f && sat <= 0.85f && mx >= 0.18f;
}

static uint32_t corner_colour(const uint8_t * px, int w, int h)
{
    int x = w / 40 + 2, y = h / 40 + 2;
    const uint8_t * p = px + ((size_t)y * w + x) * 4;      /* XRGB8888, little endian */
    return ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

static void colours_from_art(const uint8_t * px, int w, int h)
{
    if(embedded && host_colours && (host_hides_art || art_is_placeholder)) return;   /* the host's picture is under the strip (a hosted placeholder is never shown): its colours stay */
    if(art_is_placeholder) {
        /* the tape's, the record's, the text cover's own background, as it
         * is - universal (Ian, 17 Oct): the strip is the picture's ground */
        colours_set_bg(corner_colour(px, w, h), 0);
        return;
    }
    art_palette_t p;
    art_roles_t r;
    uint32_t cream = tilestyle_cream() ? tilestyle_cream() : 0xFAEFD0;
    uint32_t dark = tilestyle_dark_ink() ? tilestyle_dark_ink() : 0x29221E;
    if(!palette_extract(px, w, h, &p) || !palette_roles(&p, PAL_MODE_DOMINANT, (float)tilestyle_min_contrast(), cream, dark, 0, &r)) {
        c_bg = UI_COLOR_BG; c_ink = UI_COLOR_FG; c_dim = UI_COLOR_DIM; c_sel = ui_accent(); c_text = UI_COLOR_FG;
        set_side_colour(UI_COLOR_BG);
        apply_colours();
        return;
    }
    uint32_t bg = r.bg;
    if(skin_or_brown(bg)) {
        /* another of the cover's colours, the biggest that isn't brown or skin */
        for(int i = 0; i < p.ndom; i++) {
            uint32_t c = p.dom[i];
            if(c == r.bg || skin_or_brown(c) || p.share[i] < 4) continue;
            float cr = ((c >> 16) & 0xFF) / 255.0f, cg = ((c >> 8) & 0xFF) / 255.0f, cb = (c & 0xFF) / 255.0f;
            float chroma = fmaxf(cr, fmaxf(cg, cb)) - fminf(cr, fminf(cg, cb));
            if(chroma < 0.08f && lightness(c) > 0.5f) continue;   /* a pale grey: no better */
            bg = c;
            break;
        }
    }
    float l = lightness(bg);
    if(l > 0.22f) bg = darken(bg, 0.22f / l);           /* a dark shade of the cover's colour */
    if(lightness(bg) < 0.03f) bg = mix(bg, r.bg, 85);   /* but not black: a hint of the hue */
    c_bg = bg;
    c_ink = palette_contrast(bg, cream) >= palette_contrast(bg, dark) ? cream : dark;
    c_dim = mix(c_ink, c_bg, 70);
    /* the accent: the cover's own, vivid and readable; else the theme's */
    uint32_t cand[2] = { r.play, r.text };
    c_sel = 0;
    for(int i = 0; i < 2 && !c_sel; i++) if(cand[i] && palette_contrast(bg, cand[i]) >= 3.0 && cand[i] != c_ink && !skin_or_brown(cand[i])) c_sel = cand[i];
    if(!c_sel) {
        uint32_t play = 0, sel = 0;
        tilestyle_page_accents((unsigned)(bg ^ 0x5bd1e995u), bg, c_ink, &play, &sel);
        c_sel = (play && play != c_ink) ? play : (sel && sel != c_ink ? sel : 0);
    }
    if(!c_sel) c_sel = mix(c_ink, 0xFFFFFF, 50);        /* nothing reads: a brighter ink */
    c_text = (r.text && palette_contrast(bg, r.text) >= 3.0) ? r.text : c_ink;
    set_side_colour(darken(c_bg, 0.75f));
    apply_colours();
}

/* The page's colours from one background colour (a placeholder's own, or a
 * host's picture): ink by contrast, the accent `sel` if it reads, else a
 * theme palette colour that does. */
static void colours_set_bg(uint32_t bg, uint32_t sel_hint)
{
    uint32_t cream = tilestyle_cream() ? tilestyle_cream() : 0xFAEFD0;
    uint32_t dark = tilestyle_dark_ink() ? tilestyle_dark_ink() : 0x29221E;
    c_bg = bg;
    c_ink = palette_contrast(bg, cream) >= palette_contrast(bg, dark) ? cream : dark;
    c_dim = mix(c_ink, c_bg, 70);
    c_sel = 0;
    if(sel_hint && sel_hint != c_ink && palette_contrast(bg, sel_hint) >= 2.5 && !skin_or_brown(sel_hint)) c_sel = sel_hint;
    if(!c_sel) {
        uint32_t play = 0, sel = 0;
        tilestyle_page_accents((unsigned)(bg ^ 0x5bd1e995u), bg, c_ink, &play, &sel);
        c_sel = (play && play != c_ink) ? play : (sel && sel != c_ink ? sel : 0);
    }
    if(!c_sel) c_sel = mix(c_ink, 0xFFFFFF, 50);
    c_text = c_ink;
    set_side_colour(darken(c_bg, 0.75f));
    apply_colours();
}

static void colours_plain(void)
{
    c_bg = UI_COLOR_BG; c_ink = UI_COLOR_FG; c_dim = UI_COLOR_DIM; c_sel = ui_accent(); c_text = UI_COLOR_FG;
    set_side_colour(UI_COLOR_BG);
    if(overlay) apply_colours();
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
    if(embedded) { lv_obj_add_flag(placeholder, LV_OBJ_FLAG_HIDDEN); lv_obj_set_style_bg_opa(stage, LV_OPA_TRANSP, 0); }   /* the host has its own picture */
    else lv_obj_remove_flag(placeholder, LV_OBJ_FLAG_HIDDEN);
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
    colours_plain();
}

/* ------------------------------------------------------ side colour */

/* The colour beside the stage on a TV (and behind art that isn't square):
 * since 0.23 the strip's colour, darker, so the whole screen is one
 * picture; it fades across on a track change. */
static uint32_t side_now = 0x000000;
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
    lv_label_set_text_fmt(badge_lbl, "%d / %d", art_index + 1, art_count);
    bool flashing = !lv_timer_get_paused(badge_timer);
    bool worth = art_count > 1 || (badge_single && flashing) || badge_held;
    badge_set(worth && !host_hides_art && (controls_visible || flashing || badge_held));
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
    badge_single = false;
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
    if(host_hides_art) lv_obj_add_flag(art_img, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(art_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(placeholder, LV_OBJ_FLAG_HIDDEN);
    /* hosted: apply_host_art() follows (art_timer_cb) and decides what shows */

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
        art_is_placeholder = strcmp(r.source, "placeholder") == 0;
        art_w = r.w; art_h = r.h;
        colours_from_art(r.pixels, r.w, r.h);        /* the themed placeholder's colours too: the page matches the tape */
        set_art(&r, keep_pan);
    }
    else      { art_index = 0; art_count = 1; art_is_placeholder = true; show_placeholder(); }
    snprintf(art_shown_file, sizeof(art_shown_file), "%s", req_file);     /* the result is for the last request */
    if(embedded) apply_host_art();
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
         * one album that's the same picture, so there is no flash. Hosted
         * in the album page the picture is the page's: nothing to do. */
        if(!embedded) {
            snprintf(expected_file, sizeof(expected_file), "%s", s.file);
            enter_at_end = page_pending = false;
            if(s.file[0]) {
                if(strcmp(art_shown_file, s.file) != 0 || strcmp(req_file, s.file) != 0) {
                    snprintf(req_file, sizeof(req_file), "%s", s.file);
                    art_request(s.file, s.artist, s.album, s.title);
                }
            }
            else { art_index = 0; art_count = 1; show_placeholder(); badge_refresh(); }
        }
        last_dac_read_ms = 0;               /* re-read the DAC right away */
    }
    update_progress();
    update_format_line();
    apply_focus();                          /* play/pause, the heart, the mode */
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

/* Our own screen is the one on display (at start-up `active` can be true
 * while another screen shows; the battery disc belongs to that screen then). */
static bool on_display(void)
{
    return active && !embedded && lv_obj_get_screen(stage) == lv_screen_active();
}

static void hide_controls(void)
{
    if(!controls_visible) return;
    controls_visible = false;
    lv_obj_fade_out(overlay, FADE_MS, 0);
    lv_timer_pause(idle_timer);
    badge_refresh();
    if(on_display()) power_indicator_set_hidden(true);    /* the cover is the whole screen */
}

static void show_controls(void)
{
    lv_timer_reset(idle_timer);
    lv_timer_resume(idle_timer);
    if(controls_visible) return;
    controls_visible = true;
    lv_obj_fade_in(overlay, FADE_MS, 0);
    badge_refresh();
    if(on_display()) power_indicator_set_hidden(false);
}

static void idle_timer_cb(lv_timer_t * t)
{
    (void)t;
    hide_controls();
}

/* ------------------------------------------------------- the track list */

/* The queue as a panel over the art (the list button): what is playing
 * now - the album, the mixtape, the folder, and what play-through added
 * after it. Rows are one line each, trimmed; the selected one grows and
 * takes the accent, the one playing carries a small play mark. */
#define PANEL_MAX    600
#define PANEL_ROW_H  PX(44)
#define PANEL_TOP    PX(80)

typedef struct { char title[160], artist[96], uri[400]; int pos; } qrow_t;
static qrow_t * queue;
static int queue_n, panel_sel, panel_top;
static bool panel_open;
static lv_obj_t * panel, * panel_head, * panel_rows;

static bool queue_cb(const mpd_song_info_t * song, void * ctx)
{
    (void)ctx;
    if(queue_n >= PANEL_MAX) return false;
    qrow_t * q = &queue[queue_n++];
    const char * t = song->title[0] ? song->title : strrchr(song->uri, '/') ? strrchr(song->uri, '/') + 1 : song->uri;
    snprintf(q->title, sizeof(q->title), "%.159s", t);
    snprintf(q->artist, sizeof(q->artist), "%.95s", song->artist);
    snprintf(q->uri, sizeof(q->uri), "%.399s", song->uri);
    q->pos = song->track;
    return true;
}

static int panel_rows_y;              /* where the rows start: under the album's name, however many lines it takes */
static int panel_rows_fit(void) { return (UI_BASE - panel_rows_y - PX(16)) / PANEL_ROW_H; }

static void panel_render(void)
{
    lv_obj_clean(panel_rows);
    int fit = panel_rows_fit();
    if(panel_sel < panel_top) panel_top = panel_sel;
    if(panel_sel >= panel_top + fit) panel_top = panel_sel - fit + 1;
    if(panel_top < 0) panel_top = 0;
    const lv_font_t * f = ui_font(28), * fs = ui_font(32);
    for(int i = 0; i < fit && panel_top + i < queue_n; i++) {
        int k = panel_top + i;
        bool sel = k == panel_sel, playing = queue[k].pos == st.song && st.file[0];
        int y = i * PANEL_ROW_H;
        lv_obj_t * num = lv_label_create(panel_rows);
        lv_obj_set_style_text_font(num, sel ? fs : f, 0);
        lv_obj_set_style_text_color(num, lv_color_hex(sel ? c_sel : c_dim), 0);
        lv_obj_set_width(num, PX(44));
        lv_obj_set_style_text_align(num, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text_fmt(num, "%d", queue[k].pos + 1);
        /* a row grows from its baseline, not its centre (Ian, 17 Oct) */
        int ly = y + PANEL_ROW_H - PX(6) - lv_font_get_line_height(sel ? fs : f);
        lv_obj_set_pos(num, 0, ly);
        lv_obj_t * l = lv_label_create(panel_rows);
        lv_obj_set_style_text_font(l, sel ? fs : f, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(sel ? c_sel : c_ink), 0);
        lv_obj_set_size(l, UI_BASE - 2 * UI_MARGIN - PX(56) - PX(40), lv_font_get_line_height(sel ? fs : f));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(l, queue[k].title);
        lv_obj_set_pos(l, PX(56), ly);
        if(playing) {
            lv_obj_t * m = lv_image_create(panel_rows);
            set_icon(m, NPI_PLAY, PX(18), sel ? c_ink : c_sel, UI_BASE - 2 * UI_MARGIN - PX(12), y + PANEL_ROW_H - PX(6) - lv_font_get_line_height(f) / 2);
        }
    }
}

static void panel_open_now(void)
{
    if(!queue) queue = calloc(PANEL_MAX, sizeof(*queue));
    if(!queue) return;
    queue_n = 0;
    mpd_queue(queue_cb, NULL);
    if(queue_n == 0) { objmenu_toast(T(S_NP_NOTHING)); return; }
    panel_sel = 0;
    for(int i = 0; i < queue_n; i++) if(queue[i].pos == st.song) { panel_sel = i; break; }
    panel_top = panel_sel - panel_rows_fit() / 2;
    lv_obj_set_style_bg_color(panel, lv_color_hex(c_bg), 0);
    lv_obj_set_style_text_color(panel_head, lv_color_hex(c_text), 0);
    char head[300];
    snprintf(head, sizeof(head), "%s", st.album[0] ? st.album : (st.artist[0] ? st.artist : T(S_NP_QUEUE)));
    for(char * p = head; *p; p++) if((unsigned char)*p < 0x80) *p = (char)toupper((unsigned char)*p);
    lv_label_set_text(panel_head, head);
    lv_obj_update_layout(panel_head);          /* a long name takes two or three lines: the list moves down */
    panel_rows_y = PX(22) + lv_obj_get_height(panel_head) + PX(14);
    lv_obj_set_pos(panel_rows, UI_MARGIN, panel_rows_y);
    lv_obj_set_size(panel_rows, UI_BASE - 2 * UI_MARGIN, UI_BASE - panel_rows_y);
    panel_render();
    panel_open = true;
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(panel);
    lv_anim_delete(overlay, NULL);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);   /* the controls wait under the list */
    badge_set(false);
    lv_timer_pause(idle_timer);                /* the list stays until Back */
}

static void panel_close(void)
{
    if(!panel_open) return;
    panel_open = false;
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(overlay, LV_OPA_COVER, 0);
    controls_visible = true;
    show_controls();
    badge_refresh();
}

static void panel_action(ui_action_t a)
{
    int fit = panel_rows_fit();
    switch(a) {
        case ACT_UP:    panel_sel = panel_sel > 0 ? panel_sel - 1 : queue_n - 1; panel_render(); break;
        case ACT_DOWN:  panel_sel = panel_sel < queue_n - 1 ? panel_sel + 1 : 0; panel_render(); break;
        case ACT_LEFT:  panel_sel = panel_sel - fit < 0 ? 0 : panel_sel - fit; panel_render(); break;
        case ACT_RIGHT: panel_sel = panel_sel + fit >= queue_n ? queue_n - 1 : panel_sel + fit; panel_render(); break;
        case ACT_SELECT:
            if(mpd_play_pos(queue[panel_sel].pos)) { request_status(); panel_close(); }
            break;
        case ACT_MENU:                        /* hold A on a row: the track's menu */
            objmenu_open_track(queue[panel_sel].uri, queue[panel_sel].title, queue[panel_sel].artist, 0, -1);
            break;
        case ACT_BACK: panel_close(); break;
        default: break;
    }
}

/* ------------------------------------------------------------- build */

static lv_obj_t * make_label(lv_obj_t * parent, const lv_font_t * font, uint32_t color, int x, int y, int w)
{
    lv_obj_t * l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    lv_obj_set_height(l, lv_font_get_line_height(font));   /* one line: a long title ends in "..." (Ian, 16 Oct: it wrapped into the next row) */
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t * make_icon(lv_obj_t * parent)
{
    lv_obj_t * img = lv_image_create(parent);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    return img;
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
    lv_obj_set_style_bg_color(disc, lv_color_hex(ui_accent()), 0);
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

    /* the strip: transparent at the top, the cover's dark colour from a
     * third of the way down */
    overlay = lv_obj_create(stage);
    lv_obj_set_size(overlay, UI_BASE, OVERLAY_H);
    lv_obj_set_pos(overlay, 0, UI_BASE - OVERLAY_H);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    grad.dir = LV_GRAD_DIR_VER;
    /* two stops: lv_conf.h has LV_GRADIENT_MAX_STOPS 2; past the second
     * stop its opacity holds to the bottom edge */
    grad.stops_count = 2;
    grad.stops[0].color = lv_color_hex(c_bg); grad.stops[0].opa = LV_OPA_TRANSP; grad.stops[0].frac = 0;
    grad.stops[1].color = lv_color_hex(c_bg); grad.stops[1].opa = LV_OPA_COVER;  grad.stops[1].frac = GRAD_SOLID_FRAC;
    lv_obj_set_style_bg_grad(overlay, &grad, 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);

    const int x0 = UI_MARGIN, w = UI_BASE - 2 * UI_MARGIN;
    title_lbl  = make_label(overlay, FONT_TITLE,   c_ink, x0, Y_TITLE - PX(22),  w);
    artist_lbl = make_label(overlay, FONT_ARTIST,  c_dim, x0, Y_ARTIST - PX(16), w);
    format_lbl = make_label(overlay, UI_FONT_HINT, c_dim, x0, Y_FORMAT - PX(12), w);

    /* row 0: elapsed  [=====bar=====]  duration */
    const int time_w = PX(72);
    elapsed_lbl = make_label(overlay, UI_FONT_HINT, c_ink, x0, Y_BAR - PX(12), time_w);
    lv_label_set_text(elapsed_lbl, "0:00");
    duration_lbl = make_label(overlay, UI_FONT_HINT, c_ink, UI_BASE - UI_MARGIN - time_w, Y_BAR - PX(12), time_w);
    lv_obj_set_style_text_align(duration_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(duration_lbl, "--:--");
    bar = lv_bar_create(overlay);
    lv_obj_set_size(bar, w - 2 * PX(84), PX(6));
    lv_obj_set_pos(bar, x0 + PX(84), Y_BAR - PX(3));
    lv_obj_set_style_radius(bar, PX(3), LV_PART_MAIN);
    lv_obj_set_style_radius(bar, PX(3), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_bar_set_range(bar, 0, 1);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    knob = lv_obj_create(overlay);
    lv_obj_set_size(knob, 2 * KNOB_R, 2 * KNOB_R);
    lv_obj_set_style_radius(knob, KNOB_R, 0);
    lv_obj_set_style_border_width(knob, 0, 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    lv_obj_remove_flag(knob, LV_OBJ_FLAG_SCROLLABLE);

    /* row 1: prev  play/pause  next;  row 2: heart  mode  list  gear */
    for(int i = 0; i < TR_COUNT; i++) tr_img[i] = make_icon(overlay);
    for(int i = 0; i < BT_COUNT; i++) bt_img[i] = make_icon(overlay);

    /* page badge, top-right over the art: "2 / 11" */
    badge = lv_obj_create(stage);
    lv_obj_set_height(badge, PX(40));
    lv_obj_set_width(badge, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(badge, PX(16), 0);
    lv_obj_set_style_pad_ver(badge, 0, 0);
    lv_obj_set_style_radius(badge, PX(20), 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(c_bg), 0);
    lv_obj_set_style_bg_opa(badge, 200, 0);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -UI_MARGIN, UI_MARGIN);
    lv_obj_set_style_opa(badge, LV_OPA_TRANSP, 0);
    badge_lbl = lv_label_create(badge);
    lv_obj_set_style_text_font(badge_lbl, UI_FONT_HINT, 0);
    lv_obj_set_style_text_color(badge_lbl, lv_color_hex(c_ink), 0);
    lv_label_set_text(badge_lbl, "");
    lv_obj_center(badge_lbl);

    /* the track list panel, hidden */
    panel = lv_obj_create(stage);
    lv_obj_set_size(panel, UI_BASE, UI_BASE);
    lv_obj_set_pos(panel, 0, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);   /* the cover showed through a thinner one as a pale box */
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    panel_head = lv_label_create(panel);
    lv_obj_set_pos(panel_head, UI_MARGIN, PX(22));
    lv_obj_set_width(panel_head, UI_BASE - 2 * UI_MARGIN);
    lv_obj_set_style_text_font(panel_head, ui_font_face_px("Jost-Bold.ttf", PX(26), true), 0);
    lv_label_set_long_mode(panel_head, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(panel_head, "");
    panel_rows = lv_obj_create(panel);
    lv_obj_remove_style_all(panel_rows);              /* no card look: the theme's shadow showed as a pale box */
    lv_obj_remove_flag(panel_rows, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    panel_rows_y = PANEL_TOP;
    lv_obj_set_pos(panel_rows, UI_MARGIN, PANEL_TOP);
    lv_obj_set_size(panel_rows, UI_BASE - 2 * UI_MARGIN, UI_BASE - PANEL_TOP);

    apply_colours();
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
    ui_colours_set(c_bg, c_ink, c_sel);
    restore_playing_art();
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
    if(!embedded) power_indicator_set_hidden(false);
}

void nowplaying_mpd_changed(void)
{
    if(mpd_is_connected()) mpd_away = false;
    request_status();                       /* while hidden: fetched on show */
    if(panel_open) { lv_timer_ready(clock_timer); panel_render(); }   /* the play mark moves */
}

void nowplaying_mpd_away(void)
{
    mpd_away = true;
    if(active) update_format_line();
}

/* ---- hosted in the album page (0.17b; art only since 0.23) ---- */

/* Hosted: the art on screen is the page's own (its preview). The themed
 * placeholder is never ours to show over a host: the page has its own
 * picture for an album without a cover (11 Oct). */
static bool hosted_art_current(void)
{
    if(!embedded || host_hides_art || art_is_placeholder || !lv_image_get_src(art_img)) return false;
    return strcmp(art_shown_file, expected_file) == 0;
}

static void apply_host_art(void)
{
    bool has = lv_image_get_src(art_img) != NULL;
    bool up = embedded ? hosted_art_current() : has && !host_hides_art;
    if(embedded && !up) { lv_obj_add_flag(art_img, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(placeholder, LV_OBJ_FLAG_HIDDEN); }
    else if(host_hides_art) { lv_obj_add_flag(art_img, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(placeholder, LV_OBJ_FLAG_HIDDEN); }
    else if(has) lv_obj_remove_flag(art_img, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(placeholder, LV_OBJ_FLAG_HIDDEN);
    /* behind art that isn't square: the host's colour, not the host's own
     * picture - only while our art is actually up */
    lv_obj_set_style_bg_opa(stage, embedded && up ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    badge_refresh();
}

bool nowplaying_art_ready(void)
{
    return hosted_art_current() && !lv_obj_has_flag(art_img, LV_OBJ_FLAG_HIDDEN);
}

void nowplaying_embed(lv_obj_t * parent, int x, int y, bool with_art, uint32_t bg)
{
    embedded = true;
    host_hides_art = !with_art;
    panel_close();
    lv_obj_set_style_bg_color(stage, lv_color_hex(bg), 0);
    lv_obj_set_parent(stage, parent);
    lv_obj_set_align(stage, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(stage, x, y);
    active = true;                          /* status live for the strip; the picture never follows playback */
    row = ROW_TRANSPORT;
    tr_cur = TR_PLAY;
    lv_anim_delete(overlay, NULL);
    lv_obj_set_style_opa(overlay, LV_OPA_TRANSP, 0);
    controls_visible = false;
    status_dirty = true;
    lv_timer_resume(clock_timer);
    lv_timer_ready(clock_timer);
    lv_timer_pause(idle_timer);
    apply_host_art();
}

void nowplaying_embed_colours(uint32_t bg, uint32_t sel, bool darken_it)
{
    if(!embedded) return;
    host_colours = true;
    if(darken_it) {
        float l = lightness(bg);
        if(l > 0.22f) bg = darken(bg, 0.22f / l);
    }
    colours_set_bg(bg, sel);
}

void nowplaying_poke(void) { if(embedded) show_controls(); }
void nowplaying_hide_controls(void) { if(embedded) hide_controls(); }
bool nowplaying_controls_visible(void) { return embedded && controls_visible; }

void nowplaying_embed_art(bool with_art, uint32_t bg)
{
    if(!embedded) return;
    host_hides_art = !with_art;
    lv_obj_set_style_bg_color(stage, lv_color_hex(bg), 0);
    apply_host_art();
}

void nowplaying_unembed(void)
{
    if(!embedded) return;
    embedded = false;
    host_hides_art = false;
    active = false;
    lv_timer_pause(clock_timer);
    lv_timer_pause(idle_timer);
    if(host_colours) {
        /* back to the cover's own colours */
        host_colours = false;
        const lv_image_dsc_t * d = &art_dsc[art_slot];
        if(d->data && art_w > 0) colours_from_art(d->data, art_w, art_h); else colours_plain();
    }
    lv_anim_delete(overlay, NULL);
    lv_obj_set_style_opa(overlay, LV_OPA_COVER, 0);
    controls_visible = true;
    lv_obj_set_parent(stage, scr);
    lv_obj_center(stage);
    apply_host_art();
}

/* Hosted: show the album's own cover (11 Oct) - the first track's art,
 * the whole set, so its pages turn and the picture is placed by the same
 * rules as on the now-playing screen. The playing track's art comes back
 * when the screen itself shows (nowplaying_show). */
void nowplaying_preview(const char * uri, const char * artist, const char * album, const char * title)
{
    if(!embedded || !uri || !uri[0]) return;
    snprintf(expected_file, sizeof(expected_file), "%s", uri);
    if(strcmp(art_shown_file, uri) == 0 && lv_image_get_src(art_img)) { apply_host_art(); return; }
    snprintf(req_file, sizeof(req_file), "%s", uri);
    enter_at_end = page_pending = false;
    art_request(uri, artist, album, title);
}

/* Hosted: flash the "n / m" badge ("1 / 1" too), as the page's cover browse does. */
void nowplaying_show_badge(void)
{
    if(!embedded || host_hides_art) return;
    badge_single = true;
    badge_flash();
}

/* The art on screen (or on its way) is a page's preview of another album:
 * ask for the playing track's own again. Done when the screen itself
 * shows, the only place it matters. */
static void restore_playing_art(void)
{
    if(!cur_file[0] || strcmp(req_file, cur_file) == 0) return;
    snprintf(expected_file, sizeof(expected_file), "%s", cur_file);
    snprintf(req_file, sizeof(req_file), "%s", cur_file);
    enter_at_end = page_pending = false;
    art_request(cur_file, st.artist, st.album, st.title);
}

bool nowplaying_embedded(void) { return embedded; }

void nowplaying_browse(void)
{
    if(!embedded || host_hides_art) return;
    if(art_count <= 1 && pan_n > 0 && pan_i >= pan_n) pan_to(0);     /* one wide image: round to its left edge */
    else if(art_count > 1 || pan_n > 0) browse_art(1);
    badge_single = true;
    badge_flash();
}

int nowplaying_art_pages(void)
{
    if(host_hides_art) return 0;
    return art_count > 1 ? art_count : (pan_n > 0 ? pan_n + 1 : 1);
}

void nowplaying_browse_dir(int dir)
{
    if(!embedded || host_hides_art) return;
    browse_art(dir);
    badge_refresh();
}

void nowplaying_badge_hold(bool hold)
{
    badge_held = hold;
    if(hold) badge_single = true;
    badge_refresh();
}

void nowplaying_hide(void)
{
    active = false;
    panel_close();
    lv_timer_pause(clock_timer);
    lv_timer_pause(idle_timer);
    power_indicator_set_hidden(false);      /* whatever comes next has a header */
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

static void toggle_favorite(void)
{
    if(!st.file[0]) return;
    bool on = !curation_is_favorite(st.file);
    bool ok = curation_set_favorite(st.file, on);
    if(!ok && !curation_is_favorite(st.file)) objmenu_toast(T(S_TOAST_NO_CARD));
    else objmenu_toast(T(ok ? (on ? S_TOAST_FAVORITE : S_TOAST_UNFAVORITE) : S_TOAST_NOT_SAVED));
    apply_focus();
    if(fav_cb) fav_cb();
}

static void cycle_mode(void)
{
    play_mode_t m = (play_mode_t)((current_mode() + 1) % MODE_COUNT);
    bool ok = mpd_set_mode(m == MODE_REPEAT, m == MODE_REPEAT_ONE, m == MODE_RANDOM);
    if(!ok) return;
    st.repeat = m == MODE_REPEAT; st.single = m == MODE_REPEAT_ONE; st.random = m == MODE_RANDOM;
    static const int names[MODE_COUNT] = { S_NP_MODE_THROUGH, S_NP_MODE_REPEAT, S_NP_MODE_REPEAT_ONE, S_NP_MODE_RANDOM };
    objmenu_toast(T(names[m]));
    apply_focus();
    request_status();
}

np_result_t nowplaying_handle_action(ui_action_t a)
{
    if(!active) return NP_HANDLED;
    if(panel_open) { panel_action(a); return NP_HANDLED; }

    if(!controls_visible) {
        switch(a) {
            case ACT_BACK:  return NP_EXIT;
            case ACT_UP: case ACT_DOWN:
                show_controls();
                return NP_HANDLED;
            case ACT_SELECT:                /* as on the album page: the next picture (Ian, 17 Oct) */
                if(art_count > 1 || pan_n > 0) { browse_art(1); badge_single = true; badge_flash(); }
                else show_controls();
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
            else if(bt_cur == BT_FAV) toggle_favorite();
            else if(bt_cur == BT_MODE) cycle_mode();
            else if(bt_cur == BT_LIST) { if(!embedded) panel_open_now(); }
            else if(bt_cur == BT_MORE && st.file[0]) objmenu_open_track(st.file, st.title, st.artist, 0, -1);
            break;
        case ACT_MENU:
            if(st.file[0]) objmenu_open_track(st.file, st.title, st.artist, 0, -1);   /* hold A anywhere on the page: the track's menu */
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
