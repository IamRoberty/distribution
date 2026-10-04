/*
 * SimpletonOS UI - text fonts for every script (implementation). See fonts.h.
 *
 * ui_font() (declared in theme.h) lives here now. It takes a size in stage
 * units, scales it to the stage with PX(), and returns the head of a
 * fallback chain of FreeType fonts at exactly that pixel size; ui_font_px()
 * is the same for a size already in real pixels (layout metrics). One chain
 * per distinct size; the UI asks for four or five.
 *
 * Why the chain ends in Montserrat: LVGL's LV_SYMBOL_* strings are private-
 * use code points (U+F000...) that only the built-in fonts carry. Keeping
 * the nearest Montserrat size last means the transport icons keep working,
 * and if every .ttf were missing the UI would still have a font.
 *
 * File locations: the Noto files ship with the UI in <share>/fonts (OFL,
 * see fonts/LICENSE-Noto.txt); Noto Sans CJK is ROCKNIX's own package
 * (16 MB, one copy for the whole image) at a fixed path. A missing file is
 * logged once and simply left out of the chain.
 */
#include "fonts.h"
#include "theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHARE_DIR_DEFAULT "/usr/share/simpleton"
#define THEME_FILE        "/storage/.config/simpleton/theme"
#define THEME_DEFAULT     "pastel"
#define UIFONT_DEFAULT    "NotoSans-Medium.ttf"
#define CJK_FONT          "/usr/share/fonts/truetype/noto-cjk/NotoSansCJKsc-Regular.otf"

#define MAX_SIZES         8
#define MAX_CHAIN         24

/* The fallback chain behind the theme's typeface, in lookup order. Noto Sans
 * first because it also covers the theme font's likely gaps (accents,
 * Cyrillic) in a matching weight; then roughly by how many listeners read
 * each script, so the common lookups end early. A letter only walks the
 * chain the first time it is drawn at a size; after that it is cached. */
static const char * const chain_files[] = {
    "NotoSans-Medium.ttf",
    CJK_FONT,
    "NotoSansArabic-Regular.ttf",          /* Arabic, Farsi, Urdu */
    "NotoSansDevanagari-Regular.ttf",      /* Hindi, Marathi, Nepali */
    "NotoSansBengali-Regular.ttf",
    "NotoSansTamil-Regular.ttf",
    "NotoSansTelugu-Regular.ttf",
    "NotoSansThai-Regular.ttf",
    "NotoSansGujarati-Regular.ttf",
    "NotoSansKannada-Regular.ttf",
    "NotoSansMalayalam-Regular.ttf",
    "NotoSansGurmukhi-Regular.ttf",        /* Punjabi */
    "NotoSansMyanmar-Regular.ttf",         /* Burmese */
    "NotoSansKhmer-Regular.ttf",
    "NotoSansHebrew-Regular.ttf",
    "NotoSansOriya-Regular.ttf",           /* Odia */
    "NotoSansSinhala-Regular.ttf",
    "NotoSansLao-Regular.ttf",
    "NotoSansGeorgian-Regular.ttf",
    "NotoSansArmenian-Regular.ttf",
    "NotoSansEthiopic-Regular.ttf",        /* Amharic */
};

static bool ready;
static char share_dir[256];
static char ui_font_file[128];           /* theme's typeface, or UIFONT_DEFAULT */

static struct { int px; const lv_font_t * font; } sizes[MAX_SIZES];
static int size_count;

static bool missing_logged[sizeof(chain_files) / sizeof(chain_files[0]) + 1];

/* Built-in sizes, the pre-0.9 table: last link of every chain and the
 * whole answer when FreeType is unavailable. */
static const struct { int px; const lv_font_t * font; } builtin[] = {
    { 14, &lv_font_montserrat_14 },
    { 20, &lv_font_montserrat_20 },
    { 28, &lv_font_montserrat_28 },
    { 30, &lv_font_montserrat_30 },
    { 36, &lv_font_montserrat_36 },
    { 42, &lv_font_montserrat_42 },
    { 48, &lv_font_montserrat_48 },
};

static const lv_font_t * nearest_builtin(int px)
{
    const lv_font_t * best = builtin[0].font;
    int best_d = 1 << 30;
    for(size_t i = 0; i < sizeof(builtin) / sizeof(builtin[0]); i++) {
        int d = abs(builtin[i].px - px);
        if(d < best_d) { best_d = d; best = builtin[i].font; }
    }
    return best;
}

/* Read "uifont <file>" from the active theme; anything else keeps the default. */
static void read_theme_font(void)
{
    snprintf(ui_font_file, sizeof(ui_font_file), "%s", UIFONT_DEFAULT);

    char name[32] = THEME_DEFAULT;
    FILE * f = fopen(THEME_FILE, "r");
    if(f) {
        if(fgets(name, sizeof(name), f)) name[strcspn(name, " \r\n")] = 0;
        fclose(f);
        if(!name[0]) snprintf(name, sizeof(name), "%s", THEME_DEFAULT);
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/themes/%s.theme", share_dir, name);
    f = fopen(path, "r");
    if(!f) return;
    char line[256], file[128];
    while(fgets(line, sizeof(line), f)) {
        if(sscanf(line, "uifont %127s", file) == 1) { snprintf(ui_font_file, sizeof(ui_font_file), "%s", file); break; }
    }
    fclose(f);
}

bool fonts_init(void)
{
#if LV_USE_FREETYPE
    const char * env = getenv("SIMPLETON_SHARE");
    snprintf(share_dir, sizeof(share_dir), "%s", env && env[0] ? env : SHARE_DIR_DEFAULT);
    /* LVGL 9.5's lv_init() already starts FreeType (lv_init.c, with
     * LV_FREETYPE_CACHE_FT_GLYPH_CNT); a second lv_freetype_init() is
     * refused with a warning, so there is nothing to start here. */
    read_theme_font();
    ready = true;
    fprintf(stderr, "simpleton-ui: fonts: FreeType, UI typeface %s, Noto fallbacks\n", ui_font_file);
    return true;
#else
    fprintf(stderr, "simpleton-ui: fonts: built without FreeType, using built-in fonts\n");
    return false;
#endif
}

#if LV_USE_FREETYPE
/* Create one font of the chain at `px`, or NULL (logged once per file). */
static lv_font_t * open_font(const char * file, int px, int log_slot)
{
    char path[512];
    if(file[0] == '/') snprintf(path, sizeof(path), "%s", file);
    else               snprintf(path, sizeof(path), "%s/fonts/%s", share_dir, file);
    lv_font_t * f = lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP, (uint32_t)px,
                                            LV_FREETYPE_FONT_STYLE_NORMAL);
    if(!f && log_slot >= 0 && !missing_logged[log_slot]) {
        missing_logged[log_slot] = true;
        fprintf(stderr, "simpleton-ui: fonts: cannot load %s (left out of the chain)\n", path);
    }
    return f;
}

static const lv_font_t * build_chain(int px)
{
    lv_font_t * links[MAX_CHAIN];
    int n = 0;

    lv_font_t * head = open_font(ui_font_file, px, (int)(sizeof(chain_files) / sizeof(chain_files[0])));
    if(head) links[n++] = head;
    for(size_t i = 0; i < sizeof(chain_files) / sizeof(chain_files[0]) && n < MAX_CHAIN; i++) {
        /* the theme may name a Noto file itself: don't open it twice */
        if(strcmp(chain_files[i], ui_font_file) == 0) continue;
        lv_font_t * f = open_font(chain_files[i], px, (int)i);
        if(f) links[n++] = f;
    }
    if(n == 0) return nearest_builtin(px);

    for(int i = 0; i + 1 < n; i++) links[i]->fallback = links[i + 1];
    links[n - 1]->fallback = nearest_builtin(px);      /* LV_SYMBOL_* live here */
    return links[0];
}
#endif

const lv_font_t * ui_font(int stage_units)
{
    return ui_font_px(PX(stage_units));
}

const lv_font_t * ui_font_px(int px)
{
    if(px < 6) px = 6;
    if(!ready) return nearest_builtin(px);

    for(int i = 0; i < size_count; i++) if(sizes[i].px == px) return sizes[i].font;

    if(size_count == MAX_SIZES) {
        /* never build chains we can't keep: that would leak one per call */
        static bool warned;
        if(!warned) { warned = true; fprintf(stderr, "simpleton-ui: fonts: more than %d sizes in use, %d px gets built-in\n", MAX_SIZES, px); }
        return nearest_builtin(px);
    }
#if LV_USE_FREETYPE
    const lv_font_t * f = build_chain(px);
#else
    const lv_font_t * f = nearest_builtin(px);
#endif
    sizes[size_count].px = px;
    sizes[size_count].font = f;
    size_count++;
    return f;
}
