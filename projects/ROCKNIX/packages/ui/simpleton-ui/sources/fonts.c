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

#define UIFONT_DEFAULT    "NotoSans-Medium.ttf"
#define CJK_FONT          "/usr/share/fonts/truetype/noto-cjk/NotoSansCJKsc-Regular.otf"

#define MAX_SIZES         8         /* UI typeface sizes (the shared screens use four or five) */
#define MAX_TAILS         32        /* sizes the Noto chain has been built at */
#define MAX_FACES         256       /* (typeface, size) pairs for the theme-owned screens */
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
static struct { int px; const lv_font_t * font; } tails[MAX_TAILS];
static int tail_count;
static struct { char file[128]; int px; bool chain; const lv_font_t * font; } faces[MAX_FACES];
static int face_count;

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

    char path[512];
    config_theme_file(path, sizeof(path));
    FILE * f = fopen(path, "r");
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
    snprintf(share_dir, sizeof(share_dir), "%s", config_share_dir());
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
/* Create one font at `px`, or NULL (logged once per file). */
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

/* The Noto chain at `px` (everything behind a head typeface), ending in the
 * nearest built-in Montserrat. Built once per size and shared by every
 * head at that size, so a display face costs one FreeType font, not
 * twenty. */
static const lv_font_t * chain_tail(int px)
{
    for(int i = 0; i < tail_count; i++) if(tails[i].px == px) return tails[i].font;
    if(tail_count == MAX_TAILS) return nearest_builtin(px);

    lv_font_t * links[MAX_CHAIN];
    int n = 0;
    for(size_t i = 0; i < sizeof(chain_files) / sizeof(chain_files[0]) && n < MAX_CHAIN; i++) {
        lv_font_t * f = open_font(chain_files[i], px, (int)i);
        if(f) links[n++] = f;
    }
    const lv_font_t * tail;
    if(n == 0) tail = nearest_builtin(px);
    else {
        for(int i = 0; i + 1 < n; i++) links[i]->fallback = links[i + 1];
        links[n - 1]->fallback = nearest_builtin(px);      /* LV_SYMBOL_* live here */
        tail = links[0];
    }
    tails[tail_count].px = px;
    tails[tail_count].font = tail;
    tail_count++;
    return tail;
}

/* `head_file` at `px` with the chain behind it, or the chain alone when the
 * head can't be opened. `with_chain` false: the head on its own (a caller
 * that has checked every letter is in it). */
static const lv_font_t * build_chain(const char * head_file, int px, bool with_chain)
{
    lv_font_t * head = open_font(head_file, px, (int)(sizeof(chain_files) / sizeof(chain_files[0])));
    if(!head) return with_chain ? chain_tail(px) : nearest_builtin(px);
    head->fallback = with_chain ? chain_tail(px) : NULL;
    return head;
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
    const lv_font_t * f = build_chain(ui_font_file, px, true);
#else
    const lv_font_t * f = nearest_builtin(px);
#endif
    sizes[size_count].px = px;
    sizes[size_count].font = f;
    size_count++;
    return f;
}

const lv_font_t * ui_font_face_px(const char * file, int px, bool with_fallbacks)
{
    if(px < 6) px = 6;
    if(!ready || !file || !file[0]) return ui_font_px(px);

    for(int i = 0; i < face_count; i++)
        if(faces[i].px == px && faces[i].chain == with_fallbacks && strcmp(faces[i].file, file) == 0) return faces[i].font;

    if(face_count == MAX_FACES) {
        static bool warned;
        if(!warned) { warned = true; fprintf(stderr, "simpleton-ui: fonts: more than %d typeface sizes in use, %s %d px gets the UI face\n", MAX_FACES, file, px); }
        return ui_font_px(px);
    }
#if LV_USE_FREETYPE
    const lv_font_t * f = build_chain(file, px, with_fallbacks);
#else
    const lv_font_t * f = nearest_builtin(px);
#endif
    snprintf(faces[face_count].file, sizeof(faces[face_count].file), "%s", file);
    faces[face_count].px = px;
    faces[face_count].chain = with_fallbacks;
    faces[face_count].font = f;
    face_count++;
    return f;
}

/* One code point of UTF-8; bad bytes count as themselves. */
static uint32_t utf8_next(const char ** s)
{
    const unsigned char * p = (const unsigned char *)*s;
    uint32_t c = p[0];
    int n = 0;
    if(c >= 0xF0) { c &= 0x07; n = 3; }
    else if(c >= 0xE0) { c &= 0x0F; n = 2; }
    else if(c >= 0xC0) { c &= 0x1F; n = 1; }
    p++;
    for(int i = 0; i < n && (*p & 0xC0) == 0x80; i++, p++) c = (c << 6) | (*p & 0x3F);
    *s = (const char *)p;
    return c;
}

bool ui_font_covers(const lv_font_t * font, const char * text)
{
    const char * s = text;
    while(*s) {
        uint32_t c = utf8_next(&s);
        if(c == ' ' || c == '\n') continue;
        lv_font_glyph_dsc_t g;
        if(!lv_font_get_glyph_dsc(font, &g, c, 0) || g.is_placeholder) return false;
    }
    return true;
}
