/*
 * SimpletonOS UI - the Settings hub (implementation). See settings.h.
 *
 * A page is a short list of rows, built fresh each time it is shown from
 * the settings files, so the screen never holds a stale value:
 *
 *   page    -> opens another page            (the hub's groups and views)
 *   toggle  -> On / Off                      (Select or Left/Right flips it)
 *   choice  -> one of a few values           (Left/Right steps, Select too)
 *   action  -> does something now            (Select)
 *   text    -> a dim line, not selectable    (dividers, "nothing here yet")
 *   hub     -> "All settings"                (back to the hub)
 *
 * Rows share the browser's list measurements (L on this screen), with the
 * value on the right in the same row. The focused row is the solid block,
 * as everywhere.
 */
#include "settings.h"
#include "saver.h"
#include "classic.h"
#include "vizmenu.h"
#include "sync.h"
#include "config.h"
#include "home.h"
#include "input.h"
#include "btctl.h"
#include "objmenu.h"
#include "libs.h"
#include <dirent.h>
#include <glob.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include "library.h"
#include "mpdc.h"
#include "power.h"
#include "strings.h"
#include "textfit.h"
#include "theme.h"
#include "tilestyle.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SIMPLETON_VERSION
#define SIMPLETON_VERSION "dev"
#endif

typedef enum { ROW_PAGE, ROW_TOGGLE, ROW_CHOICE, ROW_ACTION, ROW_TEXT, ROW_HUB } row_kind_t;
typedef enum { CH_NONE, CH_THEME, CH_LANGUAGE, CH_SIZE, CH_SORT, CH_STYLE, CH_POWER, CH_LOOK, CH_NUMBER, CH_DIR, CH_STAGELOOK, CH_STAGEGLOW } choice_kind_t;

#define MAX_ROWS     32
#define MAX_CHOICES  32

typedef struct {
    row_kind_t    kind;
    char          label[128];
    char          value[96];
    char          page[32];        /* ROW_PAGE: where it goes            */
    char          key[48];         /* ROW_TOGGLE / ROW_CHOICE: the file  */
    bool          def;             /* ROW_TOGGLE: default when no file   */
    choice_kind_t choice;
    int           index;           /* ROW_CHOICE: current choice          */
    int           action;          /* ROW_ACTION                          */
    lv_obj_t *    obj;
    lv_obj_t *    val_label;
} row_t;

enum { ACT_SCAN = 1, ACT_BT_DEVICE = 2, ACT_RESET = 3, ACT_UPDATE = 4, ACT_VERSION = 5, ACT_SAVER = 6, ACT_SYNC = 7, ACT_THEME = 8, ACT_LANGUAGE = 9 };

typedef struct { char code[32]; char name[96]; } choice_t;

static lv_obj_t * scr, * header, * list_box, * footer, * sign, * title_rule;
static lv_obj_t * rules[MAX_ROWS];     /* the thin lines between groups (0.25) */
static int nrules;
static ui_list_t lm;
static row_t rows[MAX_ROWS];
static int row_count, focus;
static int hub_focus;                /* the hub row a page was opened from: Back lands on it */
static char page[32] = "hub";
static char return_to[32];            /* the page Back goes to when one was opened from another than the hub (0.32) */
static int  return_focus;             /* ...and the row it left from */
static bool from_view;
static bool active;
static void (*restart_cb)(const char * page);
static void (*view_changed_cb)(const char * view);
static choice_t choices[MAX_CHOICES];
static int choice_count;

/* ------------------------------------------------------- choice lists */

static int cmp_choice(const void * a, const void * b) { return strcmp(((const choice_t *)a)->code, ((const choice_t *)b)->code); }

/* The files <share>/<sub>/<code><ext> -> codes; `name` from a "name " line
 * (themes) or a "lang.name =" line (languages). */
static void scan_choices(const char * sub, const char * ext, bool theme_style)
{
    choice_count = 0;
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/%s", config_share_dir(), sub);
    DIR * d = opendir(dir);
    if(!d) return;
    struct dirent * e;
    size_t el = strlen(ext);
    while((e = readdir(d)) && choice_count < MAX_CHOICES) {
        size_t n = strlen(e->d_name);
        if(n <= el || strcmp(e->d_name + n - el, ext) != 0 || e->d_name[0] == '.') continue;
        choice_t * c = &choices[choice_count];
        char code[32];
        snprintf(code, sizeof(code), "%.*s", (int)(n - el), e->d_name);
        snprintf(c->code, sizeof(c->code), "%s", code);
        snprintf(c->name, sizeof(c->name), "%s", code);
        char path[600];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        FILE * f = fopen(path, "r");
        if(f) {
            char line[256];
            while(fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\r\n")] = 0;
                if(theme_style) { if(strncmp(line, "name ", 5) == 0) { snprintf(c->name, sizeof(c->name), "%.90s", line + 5); break; } }
                else {
                    char * eq = strchr(line, '=');
                    if(!eq || strncmp(line, "lang.name", 9) != 0) continue;
                    char * v = eq + 1;
                    while(*v == ' ' || *v == '\t') v++;
                    if(*v) snprintf(c->name, sizeof(c->name), "%.90s", v);
                    break;
                }
            }
            fclose(f);
        }
        choice_count++;
    }
    closedir(d);
    qsort(choices, (size_t)choice_count, sizeof(choices[0]), cmp_choice);
}

static void size_choices(void)
{
    choice_count = 0;
    for(int s = 0; s < UI_SIZE_COUNT && choice_count < MAX_CHOICES; s++) {
        if(!ui_size_offered((ui_size_t)s)) continue;
        snprintf(choices[choice_count].code, sizeof(choices[0].code), "%s", ui_size_name((ui_size_t)s));
        snprintf(choices[choice_count].name, sizeof(choices[0].name), "%s", ui_size_name((ui_size_t)s));
        choice_count++;
    }
}

static int choice_index(const char * code)
{
    for(int i = 0; i < choice_count; i++) if(strcmp(choices[i].code, code) == 0) return i;
    return 0;
}

/* The savers and visualizers, in the order the pages list them (0.32). */
static const char * const saver_codes[] = { "stage", "spectrum", "meters", "scope", "spectrogram", "liner", "drift", "cover", "palettes", "bounce" };
static const int          saver_ids[]   = { S_SET_SAVER_STAGE, S_SET_SAVER_SPECTRUM, S_SET_SAVER_METERS, S_SET_SAVER_SCOPE, S_SET_SAVER_SPECTROGRAM, S_SET_SAVER_LINER, S_SET_SAVER_DRIFT, S_SET_SAVER_COVER, S_SET_SAVER_PALETTES, S_SET_SAVER_BOUNCE };
#define NSAVERS 10
static bool saver_is_classic(const char * code) { return classic_kind_from_code(code) != CLASSIC_NONE; }
static bool saver_is_viz_code(const char * code) { return saver_kind_is_viz(saver_kind_from_code(code)); }

/* A classic's looks (classic.c) and the developer's bare number (Bounce). */
static void look_choices(void)
{
    choice_count = classic_look_count() < MAX_CHOICES ? classic_look_count() : MAX_CHOICES;
    for(int i = 0; i < choice_count; i++) { snprintf(choices[i].code, sizeof(choices[i].code), "%s", classic_look_code(i)); snprintf(choices[i].name, sizeof(choices[i].name), "%s", T((str_id_t)classic_look_name_id(i))); }
}
static void stagelook_choices(void)
{
    static const char * const c[6] = { "spectrum", "amp2000", "fluoro", "fluoro-sweep", "sunset", "twilight" };
    static const int ids[6] = { S_LOOK_STAGE_SPECTRUM, S_LOOK_AMP2000, S_LOOK_FLUORO, S_LOOK_FLUORO_SWEEP, S_LOOK_SUNSET, S_LOOK_TWILIGHT };
    choice_count = 6;
    for(int i = 0; i < 6; i++) { snprintf(choices[i].code, sizeof(choices[i].code), "%s", c[i]); snprintf(choices[i].name, sizeof(choices[i].name), "%s", T((str_id_t)ids[i])); }
}
static void stageglow_choices(void)
{
    static const char * const c[3] = { "off", "soft", "bright" };
    static const int ids[3] = { S_GLOW_OFF, S_GLOW_SOFT, S_GLOW_BRIGHT };
    choice_count = 3;
    for(int i = 0; i < 3; i++) { snprintf(choices[i].code, sizeof(choices[i].code), "%s", c[i]); snprintf(choices[i].name, sizeof(choices[i].name), "%s", T((str_id_t)ids[i])); }
}
static void dir_choices(void)
{
    choice_count = classic_spg_dir_count();
    for(int i = 0; i < choice_count; i++) { snprintf(choices[i].code, sizeof(choices[i].code), "%s", classic_spg_dir_code(i)); snprintf(choices[i].name, sizeof(choices[i].name), "%s", T((str_id_t)classic_spg_dir_name_id(i))); }
}
static void number_choices(void)
{
    static const int v[] = { 10, 50, 100, 250, 500, 1000, 2000, 5000 };
    choice_count = 8;
    for(int i = 0; i < 8; i++) { snprintf(choices[i].code, sizeof(choices[i].code), "%d", v[i]); snprintf(choices[i].name, sizeof(choices[i].name), "%d", v[i]); }
}

/* The Power page's lists (0.18): seconds or minutes as the file holds them,
 * shown as "30 s" / "2 min" / "Never". */
static void power_choices(const char * key)
{
    if(strcmp(key, "battery_show") == 0) {
        static const int ids[3] = { S_SET_NEVER, S_SET_AFTER_BUTTON, S_SET_ALWAYS };
        choice_count = 3;
        for(int i = 0; i < 3; i++) { snprintf(choices[i].code, sizeof(choices[i].code), "%d", i); snprintf(choices[i].name, sizeof(choices[i].name), "%s", T(ids[i])); }
        return;
    }
    static const int screen_off[] = { 30, 60, 120, 300, 0 };
    static const int idle_sleep[] = { 300, 600, 1800, 0 };
    static const int sleep_timer[] = { 0, 15, 30, 60, 90 };
    static const int long_min[] = { 20, 30, 60 };
    static const int dac_sleep[] = { 60, 300, 900, 1800, 0 };
    static const int sleep_off[] = { 900, 3600, 14400, 0 };
    if(strcmp(key, "tv_idle_do") == 0 || strcmp(key, "tv_music_do") == 0) {
        /* the visualizers need music: only the with-music chooser offers them (Ian, 18 Oct) */
        static const char * const codes[11] = { "off", "drift", "cover", "palettes", "bounce", "liner", "stage", "spectrum", "meters", "scope", "spectrogram" };
        static const int ids[11] = { S_SET_SAVER_OFF, S_SET_SAVER_DRIFT, S_SET_SAVER_COVER, S_SET_SAVER_PALETTES, S_SET_SAVER_BOUNCE, S_SET_SAVER_LINER, S_SET_SAVER_STAGE, S_SET_SAVER_SPECTRUM, S_SET_SAVER_METERS, S_SET_SAVER_SCOPE, S_SET_SAVER_SPECTROGRAM };
        choice_count = strcmp(key, "tv_music_do") == 0 ? 11 : 5;
        for(int i = 0; i < choice_count; i++) { snprintf(choices[i].code, sizeof(choices[i].code), "%s", codes[i]); snprintf(choices[i].name, sizeof(choices[i].name), "%s", T(ids[i])); }
        return;
    }
    static const int tv_after[] = { 60, 300, 1200, 3600, 0 };
    if(strcmp(key, "tv_idle_s") == 0 || strcmp(key, "tv_music_s") == 0) {
        choice_count = 5;
        for(int i = 0; i < 5; i++) {
            snprintf(choices[i].code, sizeof(choices[i].code), "%d", tv_after[i]);
            char num[16];
            if(tv_after[i] == 0) snprintf(choices[i].name, sizeof(choices[i].name), "%s", T(S_SET_NEVER));
            else if(tv_after[i] >= 3600) { snprintf(num, sizeof(num), "%d", tv_after[i] / 3600); ui_strf(choices[i].name, sizeof(choices[i].name), S_SET_HOURS, "n", num, NULL); }
            else { snprintf(num, sizeof(num), "%d", tv_after[i] / 60); ui_strf(choices[i].name, sizeof(choices[i].name), S_SET_MINUTES, "n", num, NULL); }
        }
        return;
    }
    if(strcmp(key, "brightness") == 0) {
        static const int pct[] = { 20, 40, 60, 80, 100 };
        choice_count = 5;
        for(int i = 0; i < 5; i++) { snprintf(choices[i].code, sizeof(choices[i].code), "%d", pct[i]); snprintf(choices[i].name, sizeof(choices[i].name), "%d%%", pct[i]); }
        return;
    }
    const int * v; int n; bool minutes;
    if(strcmp(key, "screen_off") == 0)      { v = screen_off;  n = 5; minutes = false; }
    else if(strcmp(key, "idle_sleep") == 0) { v = idle_sleep;  n = 4; minutes = false; }
    else if(strcmp(key, "sleep_timer") == 0){ v = sleep_timer; n = 5; minutes = true;  }
    else if(strcmp(key, "dac_sleep") == 0)  { v = dac_sleep;   n = 5; minutes = false; }
    else if(strcmp(key, "sleep_off") == 0)  { v = sleep_off;   n = 4; minutes = false; }
    else                                    { v = long_min;    n = 3; minutes = true;  }
    choice_count = n;
    for(int i = 0; i < n; i++) {
        snprintf(choices[i].code, sizeof(choices[i].code), "%d", v[i]);
        if(v[i] == 0) { snprintf(choices[i].name, sizeof(choices[i].name), "%s", T(strcmp(key, "sleep_timer") == 0 ? S_SET_OFF : S_SET_NEVER)); continue; }
        char num[16];
        if(!minutes && v[i] >= 3600 && v[i] % 3600 == 0) { snprintf(num, sizeof(num), "%d", v[i] / 3600); ui_strf(choices[i].name, sizeof(choices[i].name), S_SET_HOURS, "n", num, NULL); }
        else if(!minutes && v[i] >= 60 && v[i] % 60 == 0) { snprintf(num, sizeof(num), "%d", v[i] / 60); ui_strf(choices[i].name, sizeof(choices[i].name), S_SET_MINUTES, "n", num, NULL); }
        else if(!minutes) { snprintf(num, sizeof(num), "%d", v[i]); ui_strf(choices[i].name, sizeof(choices[i].name), S_SET_SECONDS, "n", num, NULL); }
        else { snprintf(num, sizeof(num), "%d", v[i]); ui_strf(choices[i].name, sizeof(choices[i].name), S_SET_MINUTES, "n", num, NULL); }
    }
}

static bool is_power_key(const char * key)
{
    return strcmp(key, "screen_off") == 0 || strcmp(key, "screen_dim") == 0 || strcmp(key, "idle_sleep") == 0 ||
           strcmp(key, "sleep_timer") == 0 || strcmp(key, "long_track_skip") == 0 || strcmp(key, "long_track_min") == 0 ||
           strcmp(key, "leds") == 0 || strcmp(key, "keep_on_plugged") == 0 || strcmp(key, "battery_show") == 0 ||
           strcmp(key, "dac_wake") == 0 || strcmp(key, "dac_sleep") == 0 || strcmp(key, "sleep_off") == 0 ||
           strcmp(key, "brightness") == 0 || strncmp(key, "tv_", 3) == 0;
}

static void add_choice(const char * label, const char * key, choice_kind_t ck, const char * current);
static void service_set(const char * unit, bool on);

static void add_power_choice(int label_id, const char * key, const char * def)
{
    char cur[32];
    power_choices(key);
    config_read(key, cur, sizeof(cur), def);
    add_choice(T(label_id), key, CH_POWER, cur);
}

/* --------------------------------------------------------- settings */

/* A view's option files: <what>-<view>-<screen kind> (the browser reads
 * the same names). */
static void view_key(const char * what, const char * view, char * out, size_t len)
{
    snprintf(out, len, "%s-%s-%s", what, view, ui_screen_kind_name());
}

/* The sorts and styles of the view whose page is on screen (`page`). */
static void sort_choices(void)
{
    if(strcmp(page, "tracks") == 0 || strcmp(page, "mixtape") == 0 || strcmp(page, "recent") == 0) {
        /* the list page's sorts (0.19) */
        static const int ids[] = { S_SORT_TITLE, S_SORT_ARTIST_TITLE, S_SORT_ARTIST_ALBUM, S_SORT_YEAR, S_SORT_NEWEST };
        static const char * const codes[] = { "title", "artist-title", "artist-year", "year", "newest" };
        choice_count = strcmp(page, "recent") == 0 ? 1 : 5;
        for(int i = 0; i < choice_count; i++) {
            snprintf(choices[i].code, sizeof(choices[i].code), "%s", strcmp(page, "recent") == 0 ? "newest" : codes[i]);
            snprintf(choices[i].name, sizeof(choices[i].name), "%s", T(strcmp(page, "recent") == 0 ? S_SORT_NEWEST : ids[i]));
        }
        return;
    }
    if(strcmp(page, "artists") == 0) {
        /* the Artists views' "Opens as" (0.20) sits in the sort row's place */
        static const int ids[] = { S_OPENS_ALBUMS, S_OPENS_TRACKS, S_OPENS_TRACKS_ALBUM };
        static const char * const codes[] = { "albums", "tracks", "tracks-album" };
        choice_count = 3;
        for(int i = 0; i < 3; i++) { snprintf(choices[i].code, sizeof(choices[i].code), "%s", codes[i]); snprintf(choices[i].name, sizeof(choices[i].name), "%s", T(ids[i])); }
        return;
    }
    if(strcmp(page, "albums") == 0) {
        static const int ids[LIB_SORT_COUNT] = { S_SORT_TITLE, S_SORT_ARTIST_TITLE, S_SORT_ARTIST_YEAR, S_SORT_YEAR, S_SORT_NEWEST };
        choice_count = LIB_SORT_COUNT;
        for(int i = 0; i < LIB_SORT_COUNT; i++) {
            snprintf(choices[i].code, sizeof(choices[i].code), "%s", library_sort_name((lib_sort_t)i));
            snprintf(choices[i].name, sizeof(choices[i].name), "%s", T(ids[i]));
        }
        return;
    }
    choice_count = 2;
    snprintf(choices[0].code, sizeof(choices[0].code), "name");
    snprintf(choices[0].name, sizeof(choices[0].name), "%s", T(S_SORT_NAME));
    snprintf(choices[1].code, sizeof(choices[1].code), "recent");
    snprintf(choices[1].name, sizeof(choices[1].name), "%s", T(S_SORT_RECENT));
}

static void style_choices(void)
{
    bool grid = strcmp(page, "albums") == 0;
    if(strcmp(page, "artists") == 0) {
        /* rows, the grid, and the list page's faces (0.20) */
        choice_count = 0;
        snprintf(choices[choice_count].code, sizeof(choices[0].code), "rows"); snprintf(choices[choice_count].name, sizeof(choices[0].name), "%s", T(S_STYLE_ROWS)); choice_count++;
        snprintf(choices[choice_count].code, sizeof(choices[0].code), "grid"); snprintf(choices[choice_count].name, sizeof(choices[0].name), "%s", T(S_STYLE_GRID)); choice_count++;
        if(ui_screen.kind != UI_SCREEN_TV) { snprintf(choices[choice_count].code, sizeof(choices[0].code), "tiles"); snprintf(choices[choice_count].name, sizeof(choices[0].name), "%s", T(S_STYLE_TILES)); choice_count++; }
        snprintf(choices[choice_count].code, sizeof(choices[0].code), "list"); snprintf(choices[choice_count].name, sizeof(choices[0].name), "%s", T(S_STYLE_LIST)); choice_count++;
        return;
    }
    if(strcmp(page, "tracks") == 0 || strcmp(page, "mixtape") == 0 || strcmp(page, "recent") == 0) {
        choice_count = 0;
        if(ui_screen.kind != UI_SCREEN_TV) { snprintf(choices[choice_count].code, sizeof(choices[0].code), "tiles"); snprintf(choices[choice_count].name, sizeof(choices[0].name), "%s", T(S_STYLE_TILES)); choice_count++; }
        snprintf(choices[choice_count].code, sizeof(choices[0].code), "list"); snprintf(choices[choice_count].name, sizeof(choices[0].name), "%s", T(S_STYLE_LIST)); choice_count++;
        if(strcmp(page, "tracks") == 0) { snprintf(choices[choice_count].code, sizeof(choices[0].code), "grid"); snprintf(choices[choice_count].name, sizeof(choices[0].name), "%s", T(S_STYLE_GRID)); choice_count++; }
        return;
    }
    choice_count = 1;
    snprintf(choices[0].code, sizeof(choices[0].code), "%s", grid ? "grid" : "list");
    snprintf(choices[0].name, sizeof(choices[0].name), "%s", T(grid ? S_STYLE_GRID : S_STYLE_LIST));
}

/* --------------------------------------------------------- the rows */

static row_t * add_row(row_kind_t kind, const char * label)
{
    if(row_count >= MAX_ROWS) return &rows[MAX_ROWS - 1];
    row_t * r = &rows[row_count++];
    memset(r, 0, sizeof(*r));
    r->kind = kind;
    snprintf(r->label, sizeof(r->label), "%s", label);
    return r;
}

static void add_page_row(const char * label, const char * to)
{
    row_t * r = add_row(ROW_PAGE, label);
    snprintf(r->page, sizeof(r->page), "%s", to);
}

static void add_toggle(const char * label, const char * key, bool def)
{
    row_t * r = add_row(ROW_TOGGLE, label);
    snprintf(r->key, sizeof(r->key), "%s", key);
    r->def = def;
    bool on = config_read_bool(key, def);
    snprintf(r->value, sizeof(r->value), "%s", T(on ? S_SET_ON : S_SET_OFF));
}

static void add_choice(const char * label, const char * key, choice_kind_t ck, const char * current)
{
    row_t * r = add_row(ROW_CHOICE, label);
    snprintf(r->key, sizeof(r->key), "%s", key);
    r->choice = ck;
    r->index = choice_index(current);
    snprintf(r->value, sizeof(r->value), "%s", choice_count ? choices[r->index].name : current);
}

static void scan_row_value(row_t * r)
{
    long stamp;
    bool updating = false;
    mpd_library_state(&stamp, &updating);
    snprintf(r->value, sizeof(r->value), "%s", updating ? T(S_SET_SCANNING) : "");
}

static const char * group_title(const char * p)
{
    if(strcmp(p, "playback") == 0) return T(S_SET_PLAYBACK);
    if(strcmp(p, "library") == 0)  return T(S_SET_LIBRARY);
    if(strcmp(p, "display") == 0)  return T(S_SET_DISPLAY);
    if(strcmp(p, "controls") == 0) return T(S_SET_CONTROLS);
    if(strcmp(p, "system") == 0)   return T(S_SET_SYSTEM);
    if(strcmp(p, "power") == 0)    return T(S_SET_POWER);
    if(strcmp(p, "btdevices") == 0) return T(S_SET_BT_DEVICES);
    if(strcmp(p, "developer") == 0) return T(S_SET_DEVELOPER);
    if(strcmp(p, "language") == 0) return T(S_SET_LANGUAGE);
    if(strcmp(p, "tv") == 0) return T(S_SET_TV);
    if(strcmp(p, "savers") == 0 || strcmp(p, "vizset") == 0) return T(S_HOME_VISUALIZATION);
    if(strncmp(p, "viz-", 4) == 0) { for(int i = 0; i < NSAVERS; i++) if(strcmp(p + 4, saver_codes[i]) == 0) return T((str_id_t)saver_ids[i]); }
    return NULL;
}

/* Settings → System (0.28): the cards, the system card's room, the update
 * file (found or not; installing it comes with the image round), a reset,
 * and the hidden Developer options behind the version row. */
static int version_presses;              /* 8 on the version row: Developer options (Ian, 28 Sep) */
static uint32_t reset_armed_ms;

static void gb_text(char * out, size_t len, unsigned long long bytes)
{
    double gb = (double)bytes / 1e9;
    if(gb >= 10) snprintf(out, len, "%.0f", gb); else snprintf(out, len, "%.1f", gb);
}

static void storage_row(const char * label, const char * path)
{
    struct statvfs st;
    row_t * r = add_row(ROW_TEXT, label);
    if(statvfs(path, &st) != 0) return;
    char fr[16], tot[16];
    gb_text(fr, sizeof(fr), (unsigned long long)st.f_bavail * st.f_frsize);
    gb_text(tot, sizeof(tot), (unsigned long long)st.f_blocks * st.f_frsize);
    ui_strf(r->value, sizeof(r->value), S_SET_FREE_OF, "free", fr, "total", tot, NULL);
}

static const char * music_dir(void)
{
    const char * e = getenv("SIMPLETON_MUSIC_DIR");
    return e && e[0] ? e : "/storage/music";
}

/* The update file on a card: SimpletonOS-<version>.update at a card's root. */
static bool find_update(char * name, size_t len)
{
    char pat[400];
    snprintf(pat, sizeof(pat), "%s/card-*/SimpletonOS-*.update", music_dir());
    glob_t g;
    bool found = false;
    if(glob(pat, 0, NULL, &g) == 0 && g.gl_pathc > 0) {
        const char * slash = strrchr(g.gl_pathv[0], '/');
        snprintf(name, len, "%s", slash ? slash + 1 : g.gl_pathv[0]);
        found = true;
    }
    globfree(&g);
    return found;
}

static void system_rows(void)
{
    row_t * r = add_row(ROW_ACTION, T(S_SET_VERSION));
    r->action = ACT_VERSION;                       /* selectable: 8 presses unlock Developer options */
    snprintf(r->value, sizeof(r->value), "%s", SIMPLETON_VERSION);
    storage_row(T(S_SET_STORAGE), "/storage");
    char upd[96];
    r = add_row(ROW_ACTION, T(S_SET_UPDATE));
    r->action = ACT_UPDATE;
    snprintf(r->value, sizeof(r->value), "%s", find_update(upd, sizeof(upd)) ? upd : T(S_SET_NO_UPDATE));
    r = add_row(ROW_ACTION, T(S_SET_RESET));
    r->action = ACT_RESET;
    if(config_read_bool("developer", false)) add_page_row(T(S_SET_DEVELOPER), "developer");
}

static void library_rows(void)
{
    add_toggle(T(S_SET_AUTOSCAN), "autoscan", true);
    add_toggle(T(S_SET_CACHE_ON_CARD), "cache_on_card", true);
    row_t * r = add_row(ROW_ACTION, T(S_SET_SCAN_NOW));
    r->action = ACT_SCAN;
    scan_row_value(r);
    add_row(ROW_TEXT, T(S_SET_CARDS));
    libs_refresh();
    int n = libs_count();
    if(n == 0) { add_row(ROW_TEXT, T(S_SET_NO_CARDS))->value[0] = ' '; return; }
    for(int i = 0; i < n; i++) {
        const lib_source_t * s = libs_get(i);
        char label[96], path[400], al[16], tr[16];
        /* the card by its label, when the mount is a link to it */
        snprintf(path, sizeof(path), "%s/%s", music_dir(), s->card);
        char target[96]; ssize_t tl = readlink(path, target, sizeof(target) - 1);
        if(tl > 0) { target[tl] = 0; const char * slash = strrchr(target, '/'); snprintf(label, sizeof(label), "%s", slash ? slash + 1 : target); }
        else snprintf(label, sizeof(label), "%s", s->card);
        r = add_row(ROW_TEXT, label);
        snprintf(al, sizeof(al), "%d", s->lib.nalbums); snprintf(tr, sizeof(tr), "%d", s->lib.ntracks);
        ui_strf(r->value, sizeof(r->value), S_SET_CARD_INFO, "albums", al, "tracks", tr, NULL);
    }
}

/* Developer options (hidden): SSH over the cable, for the workbench. */
static void developer_rows(void)
{
    add_toggle(T(S_SET_SSH), "ssh", false);
    add_row(ROW_TEXT, T(S_SET_DEV_NOTE))->value[0] = ' ';
}

/* The Bluetooth devices page (0.27): the paired ones, then the ones found
 * while the page scans; A pairs / connects / disconnects, hold A forgets. */
static const char * bt_kind_name(const bt_device_t * d)
{
    switch(d->kind) {
        case BT_KIND_GAMEPAD:  return T(S_BT_GAMEPAD);
        case BT_KIND_KEYBOARD: return T(S_BT_KEYBOARD);
        case BT_KIND_REMOTE:   return T(S_BT_REMOTE);
        case BT_KIND_POINTER:  return T(S_BT_POINTER);
        default:               return T(S_BT_OTHER);
    }
}

static void bt_rows(void)
{
    if(!bt_enabled()) { add_row(ROW_TEXT, T(S_BT_OFF)); return; }
    int n = bt_device_count(), paired = 0;
    for(int i = 0; i < n; i++) if(bt_device(i)->paired) paired++;
    if(paired) add_row(ROW_TEXT, T(S_BT_PAIRED_GROUP));
    for(int i = 0; i < n; i++) {
        const bt_device_t * d = bt_device(i);
        if(i == paired) add_row(ROW_TEXT, T(S_BT_FOUND_GROUP));
        row_t * r = add_row(ROW_ACTION, d->name);
        r->action = ACT_BT_DEVICE;
        r->index = i;
        snprintf(r->key, sizeof(r->key), "%s", d->mac);
        snprintf(r->value, sizeof(r->value), "%s", d->pairing ? T(S_BT_PAIRING_STATE) : d->connected ? T(S_BT_CONNECTED_STATE) : bt_kind_name(d));
    }
    if(n == 0 && bt_scanning()) add_row(ROW_TEXT, T(S_BT_SEARCHING));   /* only an empty page says it is looking (Ian, 18 Oct) */
}

static void build_rows(void)
{
    row_count = 0;
    if(strcmp(page, "hub") == 0) {
        add_page_row(T(S_SET_PLAYBACK), "playback");
        add_page_row(T(S_SET_LIBRARY), "library");
        add_page_row(T(S_SET_DISPLAY), "display");
        add_page_row(T(S_SET_CONTROLS), "controls");
        add_page_row(T(S_SET_POWER), "power");
        add_page_row(T(S_SET_TV), "tv");
        add_page_row(T(S_HOME_VISUALIZATION), "vizset");                  /* every saver and visualizer's look (0.32) */
        add_page_row(T(S_SET_SYSTEM), "system");
        add_row(ROW_TEXT, T(S_SET_VIEWS));
        for(int v = 0; v < HOME_VIEW_COUNT; v++) {
            if(v == HOME_NOW_PLAYING || v == HOME_SETTINGS || v == HOME_VISUALIZATION) continue;   /* the Visualization tape has its own page */
            add_page_row(home_view_name((home_view_t)v), home_view_id((home_view_t)v));
        }
        return;
    }
    if(strcmp(page, "savers") == 0) {
        /* the Home shelf's Visualization tape (0.29b): the screensavers by
         * hand, on either screen, until a press; hold A opens one's
         * options page first (Ian, 19 Oct 22:12) */
        for(int i = 0; i < NSAVERS; i++) {
            row_t * r = add_row(ROW_ACTION, T((str_id_t)saver_ids[i]));
            r->action = ACT_SAVER;
            snprintf(r->key, sizeof(r->key), "%s", saver_codes[i]);
        }
        add_row(ROW_TEXT, T(S_SET_SAVERS_NOTE))->value[0] = ' ';
        { char h[160]; ui_strf(h, sizeof(h), S_SET_SAVERS_HOLD, "a", input_letter_select(), NULL); add_row(ROW_TEXT, h)->value[0] = ' '; }   /* "Hold A: with its menu" */
        return;
    }
    if(strcmp(page, "vizset") == 0) {
        /* Settings → Visualization (0.32): the sync page, then each one's own page */
        add_row(ROW_ACTION, T(S_SYNC_TITLE))->action = ACT_SYNC;
        for(int i = 0; i < NSAVERS; i++) { char to[32]; snprintf(to, sizeof(to), "viz-%s", saver_codes[i]); add_page_row(T((str_id_t)saver_ids[i]), to); }
        return;
    }
    if(strncmp(page, "viz-", 4) == 0) {
        /* one saver or visualizer: show it, and whatever it has to set */
        const char * code = page + 4;
        bool any = false;
        if(saver_is_classic(code)) {
            any = true;
            char cur[32], key[48];
            snprintf(key, sizeof(key), "viz-%s-look", code);
            look_choices();
            config_read(key, cur, sizeof(cur), "archer");
            add_choice(T(S_SET_LOOK), key, CH_LOOK, cur);
            if(strcmp(code, "spectrogram") == 0) {
                dir_choices();
                config_read("viz-spectrogram-dir", cur, sizeof(cur), "rtl");
                add_choice(T(S_SET_DIRECTION), "viz-spectrogram-dir", CH_DIR, cur);
            }
        }
        if(strcmp(code, "stage") == 0) {
            any = true;
            char cur[32];
            stagelook_choices();
            config_read("viz-stage-look", cur, sizeof(cur), "spectrum");
            add_choice(T(S_SET_LOOK), "viz-stage-look", CH_STAGELOOK, cur);
            stageglow_choices();
            config_read("viz-stage-glow", cur, sizeof(cur), "soft");
            add_choice(T(S_SET_GLOW), "viz-stage-glow", CH_STAGEGLOW, cur);
        }
        if(strcmp(code, "bounce") == 0 && config_read_bool("developer", false)) {
            /* a number, nothing else (Ian, 19 Oct: "no explanation of what it's for") */
            any = true;
            char cur[32];
            number_choices();
            config_read("viz-bounce-n", cur, sizeof(cur), "1000");
            add_choice("", "viz-bounce-n", CH_NUMBER, cur);
        }
        if(!any) add_row(ROW_TEXT, T(S_SET_NOTHING_TO_SET))->value[0] = ' ';
        if(saver_is_viz_code(code) || strcmp(code, "liner") == 0) add_row(ROW_TEXT, T(S_SET_VIZ_MUSIC_NOTE))->value[0] = ' ';
        return;
    }
    if(strcmp(page, "playback") == 0) {
        add_toggle(T(S_SET_PLAY_THROUGH), "play_through", true);
    }
    else if(strcmp(page, "library") == 0) {
        library_rows();
    }
    else if(strcmp(page, "display") == 0) {
        /* the theme and the language are not switched as you browse (0.42,
         * Ian: "too big a change"): the theme row opens a pop-up applied on
         * Select, the language row a page of languages */
        scan_choices("themes", ".theme", true);
        { row_t * r = add_row(ROW_ACTION, T(S_SET_THEME)); r->action = ACT_THEME; snprintf(r->value, sizeof(r->value), "%s", choices[choice_index(config_theme_name())].name); }
        scan_choices("lang", ".txt", false);
        add_page_row(T(S_SET_LANGUAGE), "language");
        snprintf(rows[row_count - 1].value, sizeof(rows[row_count - 1].value), "%s", choices[choice_index(strings_language())].name);
        add_toggle(T(S_SET_HIGH_CONTRAST), "high_contrast", false);
        add_toggle(T(S_SET_HIDE_BRACKETS), "hide_brackets", false);   /* 0.41: the views without "( )" and "[ ]" (Ian) */
        add_power_choice(S_SET_BRIGHTNESS, "brightness", "100");   /* 0.28: the panel's backlight */
    }
    else if(strcmp(page, "language") == 0) {
        /* every language a row, the current one marked; Select picks it and
         * comes back to Display with the new one (0.42) */
        scan_choices("lang", ".txt", false);
        for(int i = 0; i < choice_count; i++) {
            row_t * r = add_row(ROW_ACTION, choices[i].name);
            r->action = ACT_LANGUAGE;
            snprintf(r->key, sizeof(r->key), "%s", choices[i].code);
            if(strcmp(choices[i].code, strings_language()) == 0) snprintf(r->value, sizeof(r->value), LV_SYMBOL_OK);
        }
    }
    else if(strcmp(page, "controls") == 0) {
        add_toggle(T(S_SET_SWAP_AB), "swap_ab", false);         /* 0.26a (Ian): A selects by its label; this flips A and B on every pad */
        add_toggle(T(S_SET_BLUETOOTH), "bluetooth", false);     /* 0.27: the radio, off unless wanted */
        add_page_row(T(S_SET_BT_DEVICES), "btdevices");
    }
    else if(strcmp(page, "btdevices") == 0) {
        bt_rows();
    }
    else if(strcmp(page, "power") == 0) {
        row_t * r = add_row(ROW_TEXT, T(S_SET_BATTERY));
        power_battery_text(r->value, sizeof(r->value));
        add_power_choice(S_SET_BATTERY_SHOW, "battery_show", "1");
        add_power_choice(S_SET_SCREEN_OFF, "screen_off", "60");
        add_toggle(T(S_SET_SCREEN_DIM), "screen_dim", true);
        add_toggle(T(S_SET_KEEP_ON_PLUGGED), "keep_on_plugged", true);
        add_power_choice(S_SET_IDLE_SLEEP, "idle_sleep", "600");
        add_power_choice(S_SET_SLEEP_OFF, "sleep_off", "3600");
        add_power_choice(S_SET_SLEEP_TIMER, "sleep_timer", "0");
        add_toggle(T(S_SET_LONG_SKIP), "long_track_skip", true);
        add_power_choice(S_SET_LONG_FROM, "long_track_min", "20");
        add_toggle(T(S_SET_LEDS), "leds", true);
        add_toggle(T(S_SET_DAC_WAKE), "dac_wake", true);
        add_power_choice(S_SET_DAC_SLEEP, "dac_sleep", "300");
    }
    else if(strcmp(page, "system") == 0) {
        system_rows();
    }
    else if(strcmp(page, "tv") == 0) {
        /* 0.29 (Ian, 18 Oct): two rules - nothing playing, and with music - each a time and what to show */
        add_row(ROW_TEXT, T(S_SET_STATIONARY));
        add_power_choice(S_SET_TV_IDLE_S, "tv_idle_s", "1200");
        add_power_choice(S_SET_TV_IDLE_DO, "tv_idle_do", "off");
        add_power_choice(S_SET_TV_MUSIC_S, "tv_music_s", "300");
        add_power_choice(S_SET_TV_MUSIC_DO, "tv_music_do", "drift");
        add_row(ROW_TEXT, T(S_SET_STATIONARY_NOTE))->value[0] = ' ';
    }
    else if(strcmp(page, "developer") == 0) {
        developer_rows();
    }
    else {
        /* a view's page: the picker's rows, remembered for this screen type */
        home_view_t v = home_view_from_id(page);
        if(v == HOME_FOLDERS || v == HOME_ALBUMS || v == HOME_TRACKS || v == HOME_RECENT || v == HOME_ARTISTS) {
            /* Tracks and Recent (0.19): the list page's rows; Artists (0.20): "Opens as" for the sort */
            char key[64], cur[32];
            bool list = v == HOME_TRACKS || v == HOME_RECENT;
            view_key("style", page, key, sizeof(key));
            style_choices();
            add_choice(T(S_PICKER_STYLE), key, CH_STYLE, v == HOME_ALBUMS ? "grid" : v == HOME_ARTISTS ? "rows" : (list && ui_screen.kind != UI_SCREEN_TV) ? "tiles" : "list");
            view_key("size", page, key, sizeof(key));
            size_choices();
            config_read(key, cur, sizeof(cur), ui_size_name(UI_SIZE_DEFAULT));
            add_choice(T(S_PICKER_SIZE), key, CH_SIZE, cur);
            if(v == HOME_ARTISTS) {
                view_key("opens", page, key, sizeof(key));
                sort_choices();
                config_read(key, cur, sizeof(cur), "albums");
                add_choice(T(S_SET_OPENS), key, CH_SORT, cur);
                view_key("names", page, key, sizeof(key));
                add_toggle(T(S_SET_NAMES), key, true);
                view_key("jump", page, key, sizeof(key));
                add_toggle(T(S_SET_JUMP_STRIP), key, true);
            }
            else if(v != HOME_RECENT) {
                view_key("sort", page, key, sizeof(key));
                sort_choices();
                config_read(key, cur, sizeof(cur), v == HOME_ALBUMS || v == HOME_TRACKS ? library_sort_name(LIB_SORT_TITLE) : "name");
                add_choice(T(S_PICKER_SORT), key, CH_SORT, cur);
                if(v == HOME_ALBUMS) {
                    view_key("names", page, key, sizeof(key));
                    add_toggle(T(S_SET_NAMES), key, true);          /* the name line over the covers (0.19a) */
                }
                view_key("jump", page, key, sizeof(key));
                add_toggle(T(S_SET_JUMP_STRIP), key, true);
            }
        }
        else add_row(ROW_TEXT, T(S_SET_EMPTY));
        add_row(ROW_HUB, T(S_SET_ALL));
    }
}

/* ------------------------------------------------------------ screen */

/* The visit's three colours on the chrome (0.25). */
static void paint(void)
{
    lv_obj_set_style_bg_color(scr, lv_color_hex(ui_ground()), 0);
    lv_obj_set_style_text_color(header, lv_color_hex(ui_ink()), 0);
    lv_obj_set_style_text_color(footer, lv_color_hex(ui_dim()), 0);
    ui_rule_colour(title_rule);
    ui_sign_fit(sign);
}

static void set_focus(int i)
{
    if(row_count == 0) return;
    if(i < 0) i = 0;
    if(i >= row_count) i = row_count - 1;
    focus = i;
    for(int k = 0; k < row_count; k++) {
        row_t * r = &rows[k];
        if(!r->obj) continue;
        bool on = (k == focus) && r->kind != ROW_TEXT;
        /* the selected row grows and takes the theme's accent; no bar (0.24) */
        lv_obj_set_style_bg_opa(r->obj, LV_OPA_TRANSP, 0);
        if(r->kind == ROW_TEXT && !r->value[0]) continue;          /* a group name: small caps, painted by render() */
        int px = lm.font_px;
        const lv_font_t * f = ui_font_px(on ? px * UI_GROW_PCT / 100 : px);
        lv_obj_set_style_text_font(r->obj, f, 0);
        /* the label is one line of this font, centred in the row: the
         * text grows up and down equally (0.25a, Ian) */
        lv_obj_t * l = lv_obj_get_child(r->obj, 0);
        if(l && l != r->val_label) { lv_obj_set_height(l, lv_font_get_line_height(f) + 2); lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0); }
        lv_obj_set_style_text_color(r->obj, lv_color_hex(on ? ui_accent() : (r->kind == ROW_TEXT ? ui_dim() : ui_ink())), 0);
        if(r->val_label) lv_obj_set_style_text_color(r->val_label, lv_color_hex(on ? ui_accent() : ui_dim()), 0);
    }
    if(rows[focus].obj) lv_obj_scroll_to_view(rows[focus].obj, LV_ANIM_OFF);
}

static void render(void)
{
    lv_obj_clean(list_box);
    const char * title = group_title(page);
    char buf[200];
    if(strcmp(page, "hub") == 0) title = T(S_SET_TITLE);
    else if(!title) {
        home_view_t v = home_view_from_id(page);
        ui_strf(buf, sizeof(buf), S_SET_VIEW_PAGE, "view", v < HOME_VIEW_COUNT ? home_view_name(v) : page, NULL);
        title = buf;
    }
    { char caps[200]; ui_caps(title, caps, sizeof(caps)); lv_label_set_text(header, caps); }   /* Jost Bold caps (0.25) */
    paint();
    nrules = 0;

    for(int i = 0; i < row_count; i++) {
        row_t * r = &rows[i];
        if(r->kind == ROW_TEXT && !r->value[0] && i > 0 && nrules < MAX_ROWS) {
            /* a thin rule in the ink before every group but the first (0.25) */
            lv_obj_t * line = lv_obj_create(list_box);
            lv_obj_remove_style_all(line);
            lv_obj_set_size(line, lv_obj_get_style_width(list_box, 0) - 2 * lm.margin, PX(2) < 1 ? 1 : PX(2));
            lv_obj_set_style_translate_x(line, lm.margin, 0);
            lv_obj_set_style_margin_top(line, lm.row_gap, 0);
            lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(line, lv_color_hex(ui_ink()), 0);
            rules[nrules++] = line;
        }
        lv_obj_t * row = lv_obj_create(list_box);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_obj_get_style_width(list_box, 0), r->kind == ROW_TEXT && !r->value[0] ? lm.row_h * 3 / 4 : lm.row_h);
        lv_obj_set_style_radius(row, 0, 0);                        /* square (0.25) */
        lv_obj_set_style_pad_hor(row, lm.margin, 0);
        bool group = r->kind == ROW_TEXT && !r->value[0];
        lv_obj_set_style_text_font(row, group ? ui_font_caps(lm.chrome_font_px * 85 / 100) : ui_font_px(lm.font_px), 0);
        if(group) lv_obj_set_style_text_color(row, lv_color_hex(ui_dim()), 0);   /* a group name: small caps, dimmed */
        lv_obj_t * l = lv_label_create(row);
        if(group) { char caps[128]; ui_caps(r->label, caps, sizeof(caps)); lv_label_set_text(l, caps); }
        else lv_label_set_text(l, r->label);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        /* one line tall, so a long label ends in "..." instead of wrapping */
        const lv_font_t * rf = lv_obj_get_style_text_font(row, 0);
        int lw = lv_obj_get_style_width(list_box, 0) - 2 * lm.margin;
        /* the label's width leaves room for the value as it is (plus the
         * grown face): a long card name and "531 albums · 5591 tracks"
         * overlapped at a fixed 65 % (0.40, Ian's consistency run, 16) */
        int label_w = lw;
        char txt[120] = "";
        if(r->kind == ROW_TOGGLE || r->kind == ROW_CHOICE || r->value[0]) {
            if(r->kind == ROW_CHOICE) snprintf(txt, sizeof(txt), LV_SYMBOL_LEFT " %s " LV_SYMBOL_RIGHT, r->value);
            else if(r->kind == ROW_PAGE) snprintf(txt, sizeof(txt), "%s  " LV_SYMBOL_RIGHT, r->value);   /* "English  >" (0.42) */
            else snprintf(txt, sizeof(txt), "%s", r->value);
            int vw = text_w(rf, txt) * UI_GROW_PCT / 100 + lm.margin;
            if(vw > lw * 60 / 100) vw = lw * 60 / 100;
            label_w = lw - vw;
        }
        else if(r->kind == ROW_PAGE) label_w = lw * 85 / 100;
        lv_obj_set_size(l, label_w, lv_font_get_line_height(rf) * UI_GROW_PCT / 100 + 2);   /* room for the grown face when selected */
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        r->obj = row;
        r->val_label = NULL;
        if(txt[0]) {
            lv_obj_t * v = lv_label_create(row);
            lv_label_set_text(v, txt);
            lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_width(v, lw * 60 / 100);
            lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
            r->val_label = v;
        }
        if(r->kind == ROW_PAGE && !txt[0]) {
            lv_obj_t * v = lv_label_create(row);
            lv_label_set_text(v, LV_SYMBOL_RIGHT);
            lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
            r->val_label = v;
        }
    }
    /* first selectable row */
    int f = 0;
    while(f < row_count && rows[f].kind == ROW_TEXT) f++;
    set_focus(f < row_count ? f : 0);

    char hints[300];
    const char * la = input_letter_select(), * lb = input_letter_back();     /* the hints follow Swap A and B (0.28) */
    if(strcmp(page, "btdevices") == 0) { char h[200]; ui_strf(h, sizeof(h), S_BT_HINT, "a", la, NULL); snprintf(hints, sizeof(hints), "%s  %s %s", h, lb, T(S_HINT_BACK)); }
    else snprintf(hints, sizeof(hints), "%s %s  %s %s  " LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " %s", la, T(S_HINT_SELECT), lb, T(S_HINT_BACK), T(S_HINT_CHANGE));
    lv_label_set_text(footer, hints);
}

static void open_page(const char * p)
{
    bool was_bt = strcmp(page, "btdevices") == 0, is_bt = strcmp(p, "btdevices") == 0;
    if(was_bt && !is_bt) bt_scan(false);
    snprintf(page, sizeof(page), "%s", p);
    build_rows();
    render();
    if(is_bt && !was_bt) bt_scan(true);                     /* the page scans while it is open */
}

/* The device list or a state changed (0.27): the page follows, the cursor
 * on the same device; a message or a passkey shows as a toast. */
void settings_bt_changed(void)
{
    char name[64], num[8];
    int id = bt_take_status(name, sizeof(name));
    if(id) { char t[200]; ui_strf(t, sizeof(t), (str_id_t)id, "name", name, NULL); if(active) objmenu_toast(t); }
    bt_take_passkey(num, sizeof(num));
    if(num[0] && active) { char t[200]; ui_strf(t, sizeof(t), S_BT_PASSKEY, "n", num, NULL); objmenu_toast(t); }
    if(!active || strcmp(page, "btdevices") != 0) return;
    char keep[18] = "";
    if(row_count && rows[focus].kind == ROW_ACTION && rows[focus].action == ACT_BT_DEVICE) snprintf(keep, sizeof(keep), "%s", rows[focus].key);
    build_rows();
    render();
    if(keep[0]) for(int i = 0; i < row_count; i++) if(rows[i].kind == ROW_ACTION && strcmp(rows[i].key, keep) == 0) { set_focus(i); break; }
}

/* --------------------------------------------------------- changes */

static void refresh_value(row_t * r)
{
    if(!r->val_label) return;
    char txt[120];
    if(r->kind == ROW_CHOICE) snprintf(txt, sizeof(txt), LV_SYMBOL_LEFT " %s " LV_SYMBOL_RIGHT, r->value);
    else snprintf(txt, sizeof(txt), "%s", r->value);
    lv_label_set_text(r->val_label, txt);
}

/* A key of the form <what>-<view>-<kind> belongs to a view's page. */
static bool is_view_key(const char * key)
{
    return strncmp(key, "size-", 5) == 0 || strncmp(key, "sort-", 5) == 0 ||
           strncmp(key, "jump-", 5) == 0 || strncmp(key, "style-", 6) == 0 ||
           strncmp(key, "names-", 6) == 0 || strncmp(key, "opens-", 6) == 0;
}

static void flip_toggle(row_t * r)
{
    bool on = config_read_bool(r->key, r->def);
    on = !on;
    if(!config_write(r->key, on ? "1" : "0")) fprintf(stderr, "simpleton-ui: settings: cannot write %s\n", r->key);
    snprintf(r->value, sizeof(r->value), "%s", T(on ? S_SET_ON : S_SET_OFF));
    refresh_value(r);
    if(is_view_key(r->key) && view_changed_cb) view_changed_cb(page);
    if(is_power_key(r->key)) power_reload_settings();
    if(strcmp(r->key, "swap_ab") == 0) input_reload_settings();   /* at once: the next press is already swapped */
    if(strcmp(r->key, "bluetooth") == 0) bt_set_enabled(on);        /* the service starts or stops now (0.27) */
    if(strcmp(r->key, "ssh") == 0) service_set("sshd", on);          /* developer options (0.28) */
    if(strcmp(r->key, "hide_brackets") == 0) { title_hide_brackets_set(on); if(restart_cb) restart_cb(page); }   /* every view has its titles laid out: a fresh start (0.41) */
    if(strcmp(r->key, "high_contrast") == 0) {
        /* takes effect now: the floors move, this visit is dealt again */
        tilestyle_set_high_contrast(on);
        ui_colours_redeal();
        ui_colours_visit("settings");
        int keep = focus;
        render();
        set_focus(keep);
    }
}

static void step_choice(row_t * r, int dir)
{
    if(choice_count == 0) return;
    /* the choice list on screen is the one built for this page; rebuild
     * it for this row's kind so two choice rows on a page can't mix */
    if(r->choice == CH_THEME) scan_choices("themes", ".theme", true);
    else if(r->choice == CH_LANGUAGE) scan_choices("lang", ".txt", false);
    else if(r->choice == CH_SIZE) size_choices();
    else if(r->choice == CH_SORT) sort_choices();
    else if(r->choice == CH_STYLE) style_choices();
    else if(r->choice == CH_POWER) power_choices(r->key);
    else if(r->choice == CH_LOOK) look_choices();
    else if(r->choice == CH_NUMBER) number_choices();
    else if(r->choice == CH_DIR) dir_choices();
    else if(r->choice == CH_STAGELOOK) stagelook_choices();
    else if(r->choice == CH_STAGEGLOW) stageglow_choices();
    if(choice_count < 2) return;
    r->index = (r->index + dir + choice_count) % choice_count;
    const choice_t * c = &choices[r->index];
    if(!config_write(r->key, c->code)) { fprintf(stderr, "simpleton-ui: settings: cannot write %s\n", r->key); return; }
    snprintf(r->value, sizeof(r->value), "%s", c->name);
    refresh_value(r);
    fprintf(stderr, "simpleton-ui: settings: %s = %s\n", r->key, c->code);
    /* a view's option: the view takes it now; the theme and the language
     * are read at start-up: come straight back here with the new one */
    if(is_view_key(r->key)) { if(view_changed_cb) view_changed_cb(page); }
    else if(is_power_key(r->key)) power_reload_settings();      /* takes effect now, no restart */
    else if(strncmp(r->key, "viz-", 4) == 0) { }                /* read at the next start (0.32) */
    else if(restart_cb) restart_cb(page);
}

static void service_set(const char * unit, bool on)
{
    const char * cmd = getenv("SIMPLETON_BT_SERVICE");           /* the rig's stand-in for systemctl */
    char buf[160];
    if(cmd && cmd[0]) snprintf(buf, sizeof(buf), "%s %s %s", cmd, on ? "start" : "stop", unit);
    else snprintf(buf, sizeof(buf), "systemctl %s %s 2>/dev/null", on ? "start" : "stop", unit);
    int rc = system(buf);
    fprintf(stderr, "simpleton-ui: settings: %s %s (%d)\n", unit, on ? "started" : "stopped", rc);
}

/* Reset: every setting file goes; the UI restarts on the hub with the defaults. */
static void factory_reset(void)
{
    DIR * d = opendir(config_dir());
    if(d) {
        struct dirent * e;
        while((e = readdir(d))) {
            if(e->d_name[0] == '.') continue;
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", config_dir(), e->d_name);
            unlink(path);
        }
        closedir(d);
    }
    fprintf(stderr, "simpleton-ui: settings: reset - every setting file removed\n");
    objmenu_toast(T(S_SET_RESET_DONE));
    if(restart_cb) restart_cb("hub");
}

static const char * theme_names[MAX_CHOICES];

/* the theme pop-up's pick (0.42): written, then the UI comes back on Display */
static void theme_picked(int index, void * ctx)
{
    (void)ctx;
    if(index < 0 || index >= choice_count) return;
    if(strcmp(choices[index].code, config_theme_name()) == 0) return;
    if(!config_write("theme", choices[index].code)) { fprintf(stderr, "simpleton-ui: settings: cannot write theme\n"); return; }
    fprintf(stderr, "simpleton-ui: settings: theme = %s\n", choices[index].code);
    if(restart_cb) restart_cb("display");
}

static set_result_t go_back(void);

static void run_action(row_t * r)
{
    if(r->action == ACT_RESET) {
        uint32_t now = lv_tick_get();
        if(reset_armed_ms && now - reset_armed_ms < 6000) { reset_armed_ms = 0; factory_reset(); return; }
        reset_armed_ms = now;
        char t[200]; ui_strf(t, sizeof(t), S_SET_RESET_CONFIRM, "a", input_letter_select(), NULL);
        objmenu_toast(t);
        return;
    }
    if(r->action == ACT_VERSION) {
        if(config_read_bool("developer", false)) return;
        if(++version_presses >= 8) {
            version_presses = 0;
            config_write("developer", "1");
            objmenu_toast(T(S_SET_DEV_UNLOCKED));
            int keep = focus; build_rows(); render(); set_focus(keep);
        }
        return;
    }
    if(r->action == ACT_SAVER) { saver_start_manual(saver_kind_from_code(r->key)); return; }
    if(r->action == ACT_THEME) {
        scan_choices("themes", ".theme", true);
        if(choice_count < 1) return;
        for(int i = 0; i < choice_count; i++) theme_names[i] = choices[i].name;
        objmenu_open_choice(T(S_SET_THEME), T(S_SET_THEME_HINT), theme_names, choice_count, choice_index(config_theme_name()), theme_picked, NULL);
        return;
    }
    if(r->action == ACT_LANGUAGE) {
        if(strcmp(r->key, strings_language()) == 0) { go_back(); return; }
        if(!config_write("language", r->key)) { fprintf(stderr, "simpleton-ui: settings: cannot write language\n"); return; }
        fprintf(stderr, "simpleton-ui: settings: language = %s\n", r->key);
        if(restart_cb) restart_cb("display");
        return;
    }
    if(r->action == ACT_SYNC) { sync_open(); return; }
    if(r->action == ACT_UPDATE) {
        objmenu_toast(T(S_SET_UPDATE_LATER));   /* installing comes with the image round (offline-updates-plan.md) */
        return;
    }
    if(r->action == ACT_BT_DEVICE) {
        const bt_device_t * d = bt_find(r->key);
        if(!d || d->pairing) return;
        if(!d->paired) bt_pair(d->mac);
        else if(d->connected) bt_disconnect(d->mac);
        else bt_connect(d->mac);
        settings_bt_changed();
        return;
    }
    if(r->action == ACT_SCAN) {
        if(mpd_update()) snprintf(r->value, sizeof(r->value), "%s", T(S_SET_SCANNING));
        refresh_value(r);
    }
}

/* ------------------------------------------------------------ public */

void settings_create(void (*restart)(const char *), void (*view_changed)(const char *))
{
    restart_cb = restart;
    view_changed_cb = view_changed;
    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_t * stage = ui_stage_create(scr);
    ui_list_metrics(UI_SIZE_DEFAULT, &lm);
    sign = ui_sign(stage);                                       /* the screen is a sign (0.25) */

    header = lv_label_create(stage);
    ui_sign_chrome_t sc; ui_sign_chrome(&lm, &sc);           /* inside the keyline, right of the battery corner (0.25) */
    lv_obj_set_size(header, sc.header_w, sc.header_h);
    lv_obj_set_pos(header, sc.header_x, sc.header_y);
    lv_obj_set_style_text_font(header, ui_font_caps(lm.chrome_font_px), 0);
    lv_label_set_long_mode(header, LV_LABEL_LONG_MODE_DOTS);
    title_rule = ui_rule(stage, sc.header_x, sc.rule_y, sc.header_w);

    list_box = lv_obj_create(stage);
    lv_obj_remove_style_all(list_box);
    lv_obj_set_size(list_box, sc.header_w + 2 * lm.margin, sc.list_h);   /* the rows' text on the header's edge (0.25a) */
    lv_obj_set_pos(list_box, sc.header_x - lm.margin, sc.list_y);
    lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_box, lm.row_gap, 0);
    lv_obj_set_scrollbar_mode(list_box, LV_SCROLLBAR_MODE_OFF);

    footer = lv_label_create(stage);
    lv_obj_set_size(footer, sc.header_w, lm.footer_h);
    lv_obj_set_pos(footer, sc.header_x, sc.footer_y);                 /* the hints inside the sign, on the rows' edge (0.25a) */
    lv_obj_set_style_text_font(footer, ui_font_px(lm.chrome_font_px), 0);
    lv_label_set_long_mode(footer, LV_LABEL_LONG_MODE_CLIP);
    paint();
}

void settings_show(const char * p, bool fv)
{
    active = true;
    ui_colours_visit("settings");                                /* a visit: three colours dealt once, kept inside (0.25) */
    from_view = fv;
    hub_focus = 0;                   /* a fresh visit starts at the top */
    return_to[0] = 0;
    open_page(p && p[0] ? p : "hub");
    lv_screen_load(scr);
}

void settings_hide(void)
{
    active = false;
    if(strcmp(page, "btdevices") == 0) bt_scan(false);
}

void settings_mpd_changed(void)
{
    if(!active) return;
    for(int i = 0; i < row_count; i++)
        if(rows[i].kind == ROW_ACTION && rows[i].action == ACT_SCAN) { scan_row_value(&rows[i]); refresh_value(&rows[i]); }
}

/* Back: to the page this one was opened from, else the hub, else out. */
static set_result_t go_back(void)
{
    if(return_to[0]) {
        char to[32]; snprintf(to, sizeof(to), "%s", return_to); return_to[0] = 0;
        open_page(to);
        if(return_focus > 0 && return_focus < row_count && rows[return_focus].kind != ROW_TEXT) set_focus(return_focus);
        return SET_HANDLED;
    }
    if(strcmp(page, "hub") == 0 || from_view) return SET_EXIT;
    open_page("hub");
    if(hub_focus > 0 && hub_focus < row_count && rows[hub_focus].kind != ROW_TEXT) set_focus(hub_focus);
    return SET_HANDLED;
}

set_result_t settings_handle_action(ui_action_t a)
{
    row_t * r = row_count ? &rows[focus] : NULL;
    switch(a) {
        case ACT_UP:
        case ACT_DOWN: {
            int dir = a == ACT_DOWN ? 1 : -1;
            int i = focus;
            for(int tries = 0; tries < row_count; tries++) {
                i += dir;
                if(i < 0 || i >= row_count) { i = focus; break; }       /* stop at the ends, like the browser */
                if(rows[i].kind != ROW_TEXT) break;
            }
            set_focus(i);
            return SET_HANDLED;
        }
        case ACT_LEFT:
        case ACT_RIGHT:
            if(!r) return SET_HANDLED;
            if(r->kind == ROW_TOGGLE) flip_toggle(r);
            else if(r->kind == ROW_CHOICE) step_choice(r, a == ACT_RIGHT ? 1 : -1);
            return SET_HANDLED;
        case ACT_SELECT:
            if(!r) return SET_HANDLED;
            switch(r->kind) {
                case ROW_PAGE:
                    if(strcmp(page, "hub") == 0) { hub_focus = focus; return_to[0] = 0; }
                    else { snprintf(return_to, sizeof(return_to), "%s", page); return_focus = focus; }
                    open_page(r->page); break;
                case ROW_TOGGLE: flip_toggle(r); break;
                case ROW_CHOICE: {
                    /* Select never steps a ‹ › row (Ian, 26 Oct): it means "this one" -
                     * on to the next ‹ › row below, and from the last one back to the
                     * page before, the shown value kept */
                    int next = -1;
                    for(int i = focus + 1; i < row_count; i++) if(rows[i].kind == ROW_CHOICE) { next = i; break; }
                    if(next >= 0) set_focus(next); else return go_back();
                    break;
                }
                case ROW_ACTION: run_action(r); break;
                case ROW_HUB:    from_view = false; open_page("hub"); break;
                default: break;
            }
            return SET_HANDLED;
        case ACT_MENU:                                          /* hold A on a paired device: forget it (0.27) */
            if(r && r->kind == ROW_ACTION && r->action == ACT_BT_DEVICE) {
                const bt_device_t * d = bt_find(r->key);
                if(d && d->paired) { bt_forget(d->mac); settings_bt_changed(); }
            }
            if(r && r->kind == ROW_ACTION && r->action == ACT_SAVER && strcmp(page, "savers") == 0) {
                /* hold A on the Visualization page: it starts with its menu over it (0.32c, Ian) */
                saver_kind_t k = saver_kind_from_code(r->key);
                saver_start_manual(k);
                if(saver_current() == k && vizmenu_has_options(k)) vizmenu_open(k);
            }
            return SET_HANDLED;
        case ACT_BACK:
            return go_back();
        default:
            return SET_HANDLED;
    }
}
