/*
 * SimpletonOS UI - the look of the grid's no-art tiles (implementation).
 * See tilestyle.h.
 */
#include "tilestyle.h"
#include "placeholder.h"
#include "config.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAL_MAX     128       /* palette colours kept */
#define PAIR_MAX    512       /* (bg, ink) pairs in the theme / generate pools */
#define FONT_MAX    24
#define SHARE_DEFAULT 10
#define LONG_LETTERS  18      /* a title with this many letters counts as long */
#define INKS_MAX    24        /* inks remembered per mix background */
#define GEN_COUNT   40        /* colourways invented per visit */
#define CONTRAST_DEFAULT  3.5   /* without a tilecontrast line */
#define TEXT_MIN_DEFAULT  0.12f /* without a tiletextmin line: a title never smaller than this fraction of the tile */
#define BG_MIN_LUM  0.05      /* darker than this is an ink, not a tile: it vanishes on the dark UI */
#define NEAR_SAME   28.0      /* redmean distance under which two colours count as one */
#define TOO_CLOSE   70.0      /* neighbouring tiles closer than this are skipped */

typedef struct { uint32_t bg, ink; } pair_t;

static tile_colours_t mode;
static uint32_t pal[PAL_MAX];          /* every colour, deduplicated (mix, generate) */
static int      npal;
static uint32_t set_col[PAL_MAX];      /* tilepalette colours, in file order... */
static int      set_of[PAL_MAX];       /* ...and which set each belongs to       */
static int      nset_col, nsets;
static pair_t   theme_pairs[PAIR_MAX];
static int      ntheme;
static tile_font_t fonts[FONT_MAX];
static int      nfonts;
static int      total_share;
static double   min_contrast;          /* tilecontrast: how readable an ink must be */
static double   list_contrast;         /* listcontrast (0.19): the floor for a whole list of text, a tick above the tiles' */
static double   theme_min_contrast, theme_list_contrast;   /* as the theme set them (0.25: high contrast lifts the working floors) */
static bool     high_contrast;         /* the setting (0.25, Ian: "for old people and colour blind people") */
#define HC_FLOOR 4.5                   /* the high-contrast floor: readable text by the WCAG measure */
static uint32_t cream;                 /* tilecream: the shared cream ink, 0 = none  */
static bool     has_cream;
static int      w_pal, w_cream, w_dark; /* tileinks: how often each kind of ink is drawn */
static float    text_min;              /* tiletextmin: the smallest title size, as a fraction of the tile */
static uint32_t accent;                /* the theme's accent line, 0 = none */
static const battery_style_t battery_default = { 50, 22, 10, 50, "Jost-Bold.ttf",
                                                 0x45ACC8, 0xE3D200, 0xB41823, 0x17B436, 0x29221E, 0xFAEFD0 };
static battery_style_t battery = { 50, 22, 10, 50, "Jost-Bold.ttf",
                                   0x45ACC8, 0xE3D200, 0xB41823, 0x17B436, 0x29221E, 0xFAEFD0 };   /* the theme's line over the defaults; valid before any load */
/* the album page (0.17) */
#define LAYOUT_MAX 32
static page_layout_t layouts[LAYOUT_MAX];
static int           nlayouts;
static tile_font_t   liners[FONT_MAX];
static int           nliners;
static mix_hand_t    hands[8];
static int           nhands;
static int           liner_share;
static int           ldeck[2][LAYOUT_MAX], ldeck_n[2], ldeck_pos[2];   /* layout decks: TV, handheld */

/* ----------------------------------------------------------- colour */

static double chan_lin(int c)
{
    double s = c / 255.0;
    return s <= 0.03928 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);
}

static double luminance(uint32_t rgb)
{
    return 0.2126 * chan_lin((rgb >> 16) & 255) + 0.7152 * chan_lin((rgb >> 8) & 255) + 0.0722 * chan_lin(rgb & 255);
}

double tilestyle_contrast(uint32_t a, uint32_t b)
{
    double la = luminance(a), lb = luminance(b);
    if(la < lb) { double t = la; la = lb; lb = t; }
    return (la + 0.05) / (lb + 0.05);
}

/* "redmean": a cheap perceptual distance, 0..~765 */
static double distance(uint32_t a, uint32_t b);
double tilestyle_distance(uint32_t a, uint32_t b) { return distance(a, b); }

static double distance(uint32_t a, uint32_t b)
{
    int r1 = (a >> 16) & 255, g1 = (a >> 8) & 255, b1 = a & 255;
    int r2 = (b >> 16) & 255, g2 = (b >> 8) & 255, b2 = b & 255;
    double rm = (r1 + r2) / 2.0;
    double dr = r1 - r2, dg = g1 - g2, db = b1 - b2;
    return sqrt((2 + rm / 256) * dr * dr + 4 * dg * dg + (2 + (255 - rm) / 256) * db * db);
}

static void to_hsl(uint32_t rgb, double * h, double * s, double * l)
{
    double r = ((rgb >> 16) & 255) / 255.0, g = ((rgb >> 8) & 255) / 255.0, b = (rgb & 255) / 255.0;
    double mx = fmax(r, fmax(g, b)), mn = fmin(r, fmin(g, b)), d = mx - mn;
    *l = (mx + mn) / 2;
    if(d < 1e-9) { *h = 0; *s = 0; return; }
    *s = *l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
    if(mx == r) *h = fmod((g - b) / d + 6, 6);
    else if(mx == g) *h = (b - r) / d + 2;
    else *h = (r - g) / d + 4;
    *h *= 60;
}

double tilestyle_saturation(uint32_t c)
{
    double h, s, l;
    to_hsl(c, &h, &s, &l);
    return s;
}

static double hue2(double p, double q, double t)
{
    if(t < 0) t += 1;
    if(t > 1) t -= 1;
    if(t < 1.0 / 6) return p + (q - p) * 6 * t;
    if(t < 0.5) return q;
    if(t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
}

static uint32_t from_hsl(double h, double s, double l)
{
    h = fmod(fmod(h, 360) + 360, 360) / 360;
    if(s < 0) s = 0;
    if(s > 1) s = 1;
    if(l < 0) l = 0;
    if(l > 1) l = 1;
    double r, g, b;
    if(s == 0) r = g = b = l;
    else {
        double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
        r = hue2(p, q, h + 1.0 / 3); g = hue2(p, q, h); b = hue2(p, q, h - 1.0 / 3);
    }
    return ((uint32_t)lround(r * 255) << 16) | ((uint32_t)lround(g * 255) << 8) | (uint32_t)lround(b * 255);
}

/* ----------------------------------------------------------- random */

typedef struct { uint32_t s; } rng_t;

static uint32_t rnd(rng_t * r)
{
    uint32_t x = r->s ? r->s : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    r->s = x;
    return x;
}

static double rndf(rng_t * r) { return (rnd(r) >> 8) / 16777216.0; }
static double rndr(rng_t * r, double a, double b) { return a + (b - a) * rndf(r); }

static void shuffle(int * a, int n, rng_t * r)
{
    for(int i = n - 1; i > 0; i--) { int j = (int)(rnd(r) % (uint32_t)(i + 1)); int t = a[i]; a[i] = a[j]; a[j] = t; }
}

/* ----------------------------------------------------------- loading */

static bool parse_hex(const char * s, uint32_t * out)
{
    if(*s == '#') s++;
    char * end;
    unsigned long v = strtoul(s, &end, 16);
    if(end - s != 6) return false;
    *out = (uint32_t)v;
    return true;
}

static void pal_add(uint32_t c)
{
    for(int i = 0; i < npal; i++) if(distance(pal[i], c) < NEAR_SAME) return;
    if(npal < PAL_MAX) pal[npal++] = c;
}

/* A font line's options after the file name and caps|mixed; shared by
 * coverfont and linerfont. Returns false without a file name. */
static bool parse_font_line(char * rest, tile_font_t * f)
{
    memset(f, 0, sizeof(*f));
    f->leading = 100;
    f->share = SHARE_DEFAULT;
    f->max_letters = 0;
    char * save;
    int k = 0;
    bool ok = false;
    for(char * tok = strtok_r(rest, " \t\n", &save); tok; tok = strtok_r(NULL, " \t\n", &save), k++) {
        int v; double dv;
        if(k == 0) { snprintf(f->file, sizeof(f->file), "%s", tok); ok = true; }
        else if(k == 1) f->caps = strcmp(tok, "caps") == 0;
        else if(sscanf(tok, "leading=%d", &v) == 1) f->leading = v < 30 ? 30 : v > 200 ? 200 : v;
        else if(sscanf(tok, "share=%d", &v) == 1) f->share = v < 0 ? 0 : v > 100 ? 100 : v;
        else if(sscanf(tok, "max=%d", &v) == 1) f->max_letters = v < 1 ? 1 : v;
        else if(sscanf(tok, "min=%lf", &dv) == 1) f->min_size = (float)(dv < 0 ? 0 : dv > 0.5 ? 0.5 : dv);
        else if(sscanf(tok, "contrast=%lf", &dv) == 1) f->contrast = dv < 1 ? 1 : dv;
        else if(strcmp(tok, "long") == 0) f->for_long = true;
        else if(strcmp(tok, "big") == 0) f->big = true;
    }
    return ok;
}

/* "any" (or nothing) leaves a font field empty: dealt per visit. */
static void font_field(char * out, size_t len, const char * v)
{
    if(strcmp(v, "any") == 0) out[0] = 0;
    else snprintf(out, len, "%s", v);
}

static void parse_layout(char * rest)
{
    if(nlayouts >= LAYOUT_MAX) return;
    page_layout_t * l = &layouts[nlayouts];
    memset(l, 0, sizeof(*l));
    l->size = 'M';
    l->header = PAGE_HEADER_ARTIST_BIG;
    bool screen_set = false;
    char * save;
    int k = 0;
    for(char * tok = strtok_r(rest, " \t\n", &save); tok; tok = strtok_r(NULL, " \t\n", &save), k++) {
        if(k == 0) { snprintf(l->name, sizeof(l->name), "%s", tok); continue; }
        char * eq = strchr(tok, '=');
        if(!eq) continue;
        *eq = 0;
        const char * v = eq + 1;
        if(strcmp(tok, "screen") == 0) {
            if(strcmp(v, "tv") == 0) { l->screen = PAGE_SCREEN_TV; screen_set = true; }
            else if(strcmp(v, "handheld") == 0) { l->screen = PAGE_SCREEN_HANDHELD; screen_set = true; }
        }
        else if(strcmp(tok, "art") == 0)
            l->art = strcmp(v, "right") == 0 ? PAGE_ART_RIGHT : (strcmp(v, "centre") == 0 || strcmp(v, "center") == 0) ? PAGE_ART_CENTRE : PAGE_ART_LEFT;
        else if(strcmp(tok, "sides") == 0)
            l->sides = strcmp(v, "letters") == 0 ? PAGE_SIDES_LETTERS : strcmp(v, "numbers") == 0 ? PAGE_SIDES_NUMBERS : PAGE_SIDES_NONE;
        else if(strcmp(tok, "header") == 0)
            l->header = strcmp(v, "album-big") == 0 ? PAGE_HEADER_ALBUM_BIG : strcmp(v, "centred") == 0 || strcmp(v, "centered") == 0 ? PAGE_HEADER_CENTRED
                      : strcmp(v, "none") == 0 ? PAGE_HEADER_NONE : PAGE_HEADER_ARTIST_BIG;
        else if(strcmp(tok, "artist") == 0) font_field(l->artist, sizeof(l->artist), v);
        else if(strcmp(tok, "album") == 0) font_field(l->album, sizeof(l->album), v);
        else if(strcmp(tok, "liner") == 0) font_field(l->liner, sizeof(l->liner), v);
        else if(strcmp(tok, "size") == 0) l->size = v[0] == 'S' || v[0] == 's' ? 'S' : v[0] == 'L' || v[0] == 'l' ? 'L' : 'M';
        else if(strcmp(tok, "leading") == 0) { int n = atoi(v); l->leading = n < 30 ? 0 : n > 200 ? 200 : n; }
        else if(strcmp(tok, "numbers") == 0)
            l->numbers = strcmp(v, "01") == 0 ? PAGE_NUMBERS_PADDED : strcmp(v, "1.") == 0 ? PAGE_NUMBERS_DOT : strcmp(v, "1") == 0 ? PAGE_NUMBERS_PLAIN : PAGE_NUMBERS_NONE;
        else if(strcmp(tok, "align") == 0)
            l->align = strcmp(v, "centre") == 0 || strcmp(v, "center") == 0 ? PAGE_ALIGN_CENTRE : strcmp(v, "right") == 0 ? PAGE_ALIGN_RIGHT : PAGE_ALIGN_LEFT;
        else if(strcmp(tok, "colours") == 0 || strcmp(tok, "colors") == 0)
            l->colours = strcmp(v, "paper") == 0 ? PAGE_COLOURS_PAPER : strcmp(v, "tile") == 0 ? PAGE_COLOURS_TILE : PAGE_COLOURS_ART;
    }
    if(l->name[0] && screen_set) nlayouts++;
}

static void apply_floors(void)
{
    min_contrast = high_contrast && theme_min_contrast < HC_FLOOR ? HC_FLOOR : theme_min_contrast;
    double lc = theme_list_contrast > 0 ? theme_list_contrast : theme_min_contrast + 1.0;
    list_contrast = high_contrast && lc < HC_FLOOR ? HC_FLOOR : lc;
}
void tilestyle_set_high_contrast(bool on) { high_contrast = on; apply_floors(); }
bool tilestyle_high_contrast(void) { return high_contrast; }
int  tilestyle_set_count(void) { return nsets; }
int  tilestyle_set(int set, uint32_t * out, int max)
{
    int n = 0;
    for(int j = 0; j < nset_col && n < max; j++) if(set_of[j] == set) out[n++] = set_col[j];
    return n;
}

void tilestyle_load(const char * theme_path)
{
    mode = TILE_COLOURS_THEME;
    npal = ntheme = nfonts = nset_col = nsets = total_share = 0;
    nlayouts = nliners = liner_share = 0; nhands = 0;
    ldeck_n[0] = ldeck_n[1] = ldeck_pos[0] = ldeck_pos[1] = 0;
    accent = 0;
    battery = battery_default;
    theme_min_contrast = CONTRAST_DEFAULT;
    theme_list_contrast = 0;
    has_cream = false;
    w_pal = 100; w_cream = 0; w_dark = 0;
    text_min = TEXT_MIN_DEFAULT;
    FILE * f = theme_path ? fopen(theme_path, "r") : NULL;
    char line[512];
    while(f && fgets(line, sizeof(line), f)) {
        char * semi = strchr(line, ';');
        if(semi) *semi = 0;
        char a[64];
        if(sscanf(line, "tilecolours %63s", a) == 1) {
            if(strcmp(a, "palettes") == 0) mode = TILE_COLOURS_PALETTES;
            else if(strcmp(a, "mix") == 0) mode = TILE_COLOURS_MIX;
            else if(strcmp(a, "generate") == 0) mode = TILE_COLOURS_GENERATE;
            else mode = TILE_COLOURS_THEME;
        }
        else if(strncmp(line, "tilecontrast", 12) == 0) {
            double v = atof(line + 12);
            if(v >= 1.0 && v <= 21.0) theme_min_contrast = v;
        }
        else if(strncmp(line, "listcontrast", 12) == 0) {
            double v = atof(line + 12);
            if(v >= 1.0 && v <= 21.0) theme_list_contrast = v;
        }
        else if(strncmp(line, "tiletextmin", 11) == 0) {
            double v = atof(line + 11);
            if(v >= 0.04 && v <= 0.5) text_min = (float)v;
        }
        else if(sscanf(line, "tilecream %63s", a) == 1) {
            has_cream = parse_hex(a, &cream);
        }
        else if(strncmp(line, "tileinks", 8) == 0) {
            char * save;
            for(char * tok = strtok_r(line + 8, " \t\n", &save); tok; tok = strtok_r(NULL, " \t\n", &save)) {
                int v;
                if(sscanf(tok, "palette=%d", &v) == 1) w_pal = v < 0 ? 0 : v;
                else if(sscanf(tok, "cream=%d", &v) == 1) w_cream = v < 0 ? 0 : v;
                else if(sscanf(tok, "dark=%d", &v) == 1) w_dark = v < 0 ? 0 : v;
            }
        }
        else if(strncmp(line, "tilepalette", 11) == 0) {
            char * save;
            char * tok = strtok_r(line + 11, " \t\n", &save);             /* the set's name */
            bool any = false;
            for(tok = tok ? strtok_r(NULL, " \t\n", &save) : NULL; tok; tok = strtok_r(NULL, " \t\n", &save)) {
                uint32_t rgb;
                if(!parse_hex(tok, &rgb)) continue;
                pal_add(rgb);
                if(nset_col < PAL_MAX) { set_col[nset_col] = rgb; set_of[nset_col] = nsets; nset_col++; any = true; }
            }
            if(any) nsets++;
        }
        else if(strncmp(line, "coverfont", 9) == 0 && nfonts < FONT_MAX) {
            tile_font_t * f = &fonts[nfonts];
            if(parse_font_line(line + 9, f) && f->share > 0) { total_share += f->share; nfonts++; }
        }
        else if(strncmp(line, "linerfont", 9) == 0 && nliners < FONT_MAX) {
            tile_font_t * f = &liners[nliners];
            if(parse_font_line(line + 9, f) && f->share > 0) { liner_share += f->share; nliners++; }
        }
        else if(strncmp(line, "mixhand", 7) == 0 && nhands < 8) {
            mix_hand_t * h = &hands[nhands]; memset(h, 0, sizeof(*h)); h->size_pct = 100;
            char * save; int k = 0, v;
            for(char * tok = strtok_r(line + 7, " \t\n", &save); tok; tok = strtok_r(NULL, " \t\n", &save), k++) {
                if(k == 0) snprintf(h->file, sizeof(h->file), "%s", tok);
                else if(strcmp(tok, "upper") == 0) h->letter_case = HAND_UPPER;
                else if(strcmp(tok, "lower") == 0) h->letter_case = HAND_LOWER;
                else if(sscanf(tok, "size=%d", &v) == 1) h->size_pct = v < 50 ? 50 : v > 200 ? 200 : v;
            }
            if(h->file[0]) nhands++;
        }
        else if(strncmp(line, "pagelayout", 10) == 0) parse_layout(line + 10);
        else if(sscanf(line, "accent %63s", a) == 1) {
            if(!parse_hex(a, &accent)) accent = 0;
        }
        else if(strncmp(line, "battery", 7) == 0 && (line[7] == ' ' || line[7] == '\t')) {
            char * save;
            for(char * tok = strtok_r(line + 7, " \t\n", &save); tok; tok = strtok_r(NULL, " \t\n", &save)) {
                char * eq = strchr(tok, '=');
                if(!eq) continue;
                *eq = 0;
                const char * v = eq + 1;
                uint32_t rgb;
                if(strcmp(tok, "diameter") == 0)       battery.diameter = atoi(v) > 0 ? atoi(v) : battery.diameter;
                else if(strcmp(tok, "size") == 0)      battery.size = atoi(v) > 0 ? atoi(v) : battery.size;
                else if(strcmp(tok, "mid_from") == 0)  battery.mid_from = atoi(v);
                else if(strcmp(tok, "high_from") == 0) battery.high_from = atoi(v);
                else if(strcmp(tok, "font") == 0)      snprintf(battery.font, sizeof(battery.font), "%s", v);
                else if(!parse_hex(v, &rgb)) continue;
                else if(strcmp(tok, "high") == 0)      battery.high = rgb;
                else if(strcmp(tok, "mid") == 0)       battery.mid = rgb;
                else if(strcmp(tok, "low") == 0)       battery.low = rgb;
                else if(strcmp(tok, "charge") == 0)    battery.charge = rgb;
                else if(strcmp(tok, "ink") == 0)       battery.ink = rgb;
                else if(strcmp(tok, "inklight") == 0)  battery.ink_light = rgb;
            }
        }
        else if(strncmp(line, "colourway", 9) == 0 || strncmp(line, "tileway", 7) == 0) {
            bool tileway = line[0] == 't';
            uint32_t label = 0, ink = 0, bg = 0;
            bool has_label = false, has_ink = false, has_bg = false;
            char * save;
            for(char * tok = strtok_r(line + (tileway ? 7 : 9), " \t\n", &save); tok; tok = strtok_r(NULL, " \t\n", &save)) {
                char * eq = strchr(tok, '=');
                uint32_t rgb;
                if(!eq || !parse_hex(eq + 1, &rgb)) continue;
                *eq = 0;
                pal_add(rgb);
                if(strcmp(tok, "text") == 0 || strcmp(tok, "ink") == 0) { ink = rgb; has_ink = true; }
                else if(strcmp(tok, "label") == 0) { label = rgb; has_label = true; }
                else if(strcmp(tok, "bg") == 0) { bg = rgb; has_bg = true; }
            }
            uint32_t back = tileway ? bg : label;
            if((tileway ? has_bg : has_label) && has_ink && ntheme < PAIR_MAX)
                theme_pairs[ntheme++] = (pair_t){ back, ink };
        }
    }
    if(f) fclose(f);
    high_contrast = config_read_bool("high_contrast", false);
    apply_floors();
    fprintf(stderr, "simpleton-ui: tiles: colours %s, %d colourways, %d sets (%d colours), %d palette colours, "
            "contrast %.1f, inks palette %d cream %d dark %d, %d cover fonts; page: %d layouts, %d liner fonts\n",
            tilestyle_mode_name(), ntheme, nsets, nset_col, npal, min_contrast, w_pal, has_cream ? w_cream : 0, w_dark, nfonts,
            nlayouts, nliners);
}

double   tilestyle_min_contrast(void) { return min_contrast; }
double   tilestyle_list_contrast(void) { return list_contrast; }

void tilestyle_deal_list(unsigned seed, int n, tile_style_t * out)
{
    if(n <= 0) return;
    bool * noart = malloc((size_t)n * sizeof(bool));
    if(!noart) return;
    for(int i = 0; i < n; i++) noart[i] = true;
    double keep = min_contrast;
    min_contrast = tilestyle_list_contrast();      /* the pool and the inks are built per deal */
    tilestyle_deal(seed, n, 1, noart, NULL, out);
    min_contrast = keep;
    free(noart);
}
uint32_t tilestyle_cream(void)        { return has_cream ? cream : 0; }
uint32_t tilestyle_accent(void)       { return accent; }
const battery_style_t * tilestyle_battery(void) { return &battery; }

uint32_t tilestyle_dark_ink(void)
{
    uint32_t best = 0x202020;
    double bl = 1;
    for(int i = 0; i < npal; i++) { double l = luminance(pal[i]); if(l < bl) { bl = l; best = pal[i]; } }
    return best;
}

int tilestyle_font_index(const char * file)
{
    if(!file || !file[0]) return -1;
    for(int i = 0; i < nfonts; i++) if(strcmp(fonts[i].file, file) == 0) return i;
    return -1;
}

int                   tilestyle_layout_count(void) { return nlayouts; }
const page_layout_t * tilestyle_layout(int i)      { return i >= 0 && i < nlayouts ? &layouts[i] : NULL; }
int                   tilestyle_liner_count(void)  { return nliners; }
const tile_font_t *   tilestyle_liner(int i)       { return i >= 0 && i < nliners ? &liners[i] : NULL; }

int tilestyle_liner_index(const char * file)
{
    if(!file || !file[0]) return -1;
    for(int i = 0; i < nliners; i++) if(strcmp(liners[i].file, file) == 0) return i;
    return -1;
}

tile_colours_t tilestyle_mode(void) { return mode; }

const char * tilestyle_mode_name(void)
{
    return mode == TILE_COLOURS_PALETTES ? "palettes" : mode == TILE_COLOURS_MIX ? "mix" : mode == TILE_COLOURS_GENERATE ? "generate" : "theme";
}

float tilestyle_text_min(void) { return text_min; }

int tilestyle_font_count(void) { return nfonts; }

const tile_font_t * tilestyle_font(int i)
{
    return i >= 0 && i < nfonts ? &fonts[i] : NULL;
}

bool tilestyle_font_fits(int i, int letters, uint32_t bg, uint32_t ink)
{
    const tile_font_t * f = tilestyle_font(i);
    if(!f) return false;
    if(f->max_letters && letters > f->max_letters) return false;
    if(f->contrast > 0 && tilestyle_contrast(bg, ink) < f->contrast) return false;
    return true;
}

/* ----------------------------------------------------------- pools */

/* A tile colour and the inks it may take, by kind (Ian, 9 Oct: "a lot with
 * colours from their palette", cream common, some off-black; Archer Ave. is
 * about vibe more than contrast, so the floor is the theme's tilecontrast). */
typedef struct {
    uint32_t bg;
    uint32_t col[INKS_MAX];  int ncol;     /* palette colours that read on it */
    uint32_t dark[INKS_MAX]; int ndark;    /* off-blacks that read on it      */
    bool     cream_ok;                     /* the cream reads, and it isn't pastel */
} mixbg_t;

static bool is_dark(uint32_t c) { return luminance(c) < BG_MIN_LUM; }

static bool reads(uint32_t bg, uint32_t ink)
{
    return tilestyle_contrast(bg, ink) >= min_contrast && distance(bg, ink) >= TOO_CLOSE;
}

/* Fill m's inks. `same_set`: the set to draw palette colours from, -1 any.
 * Off-blacks come from the same set when it has one that reads, else any. */
static void collect_inks(mixbg_t * m, int same_set)
{
    m->ncol = m->ndark = 0;
    if(same_set >= 0) {
        for(int j = 0; j < nset_col; j++) {
            if(set_of[j] != same_set || !reads(m->bg, set_col[j])) continue;
            if(is_dark(set_col[j])) { if(m->ndark < INKS_MAX) m->dark[m->ndark++] = set_col[j]; }
            else if(m->ncol < INKS_MAX) m->col[m->ncol++] = set_col[j];
        }
    }
    if(m->ncol == 0)                     /* any set, also when its own set has nothing that reads */
        for(int j = 0; j < npal && m->ncol < INKS_MAX; j++)
            if(!is_dark(pal[j]) && reads(m->bg, pal[j])) m->col[m->ncol++] = pal[j];
    if(m->ndark == 0)
        for(int j = 0; j < npal && m->ndark < INKS_MAX; j++)
            if(is_dark(pal[j]) && reads(m->bg, pal[j])) m->dark[m->ndark++] = pal[j];
    double h, s, l;
    to_hsl(m->bg, &h, &s, &l);
    m->cream_ok = has_cream && l < 0.80 && reads(m->bg, cream);
}

/* The colour's own hue taken deep or cream, whichever reads better: the
 * last resort when nothing in the palette reads on it. */
static uint32_t own_ink(uint32_t bg)
{
    double h, s, l;
    to_hsl(bg, &h, &s, &l);
    uint32_t deep = from_hsl(h, s < 0.25 ? 0.25 : s > 0.7 ? 0.7 : s, 0.16);
    uint32_t pale = from_hsl(h, 0.5, 0.94);
    return tilestyle_contrast(bg, deep) >= tilestyle_contrast(bg, pale) ? deep : pale;
}

/* Draw one ink by the theme's weights, over the kinds this tile can take. */
static uint32_t pick_ink(const mixbg_t * m, rng_t * r)
{
    int wp = m->ncol ? w_pal : 0, wc = m->cream_ok ? w_cream : 0, wd = m->ndark ? w_dark : 0;
    int tot = wp + wc + wd;
    if(tot <= 0) {                       /* the weights rule out everything it can take */
        if(m->ncol) return m->col[rnd(r) % (uint32_t)m->ncol];
        if(m->cream_ok) return cream;
        if(m->ndark) return m->dark[rnd(r) % (uint32_t)m->ndark];
        return own_ink(m->bg);
    }
    int x = (int)(rnd(r) % (uint32_t)tot);
    if(x < wp) return m->col[rnd(r) % (uint32_t)m->ncol];
    if(x < wp + wc) return cream;
    return m->dark[rnd(r) % (uint32_t)m->ndark];
}

/* mix: any palette colour as the tile, inks from anywhere */
static int build_mix(mixbg_t * out)
{
    int n = 0;
    for(int i = 0; i < npal; i++) {
        if(is_dark(pal[i])) continue;
        out[n].bg = pal[i];
        collect_inks(&out[n], -1);
        n++;
    }
    return n;
}

/* palettes: every set colour, inked from its own set */
static int build_palettes(mixbg_t * out)
{
    int n = 0;
    for(int i = 0; i < nset_col; i++) {
        if(is_dark(set_col[i])) continue;
        out[n].bg = set_col[i];
        collect_inks(&out[n], set_of[i]);
        n++;
    }
    return n;
}

/* generate: colourways invented around the palette */
static int build_generated(mixbg_t * out, rng_t * r)
{
    double hues[PAL_MAX];
    int nh = 0;
    double smin = 1, smax = 0, lmin = 1, lmax = 0;
    for(int i = 0; i < npal; i++) {
        double h, s, l;
        to_hsl(pal[i], &h, &s, &l);
        if(s < 0.15 || l < 0.12 || l > 0.92) continue;        /* greys, near-blacks, creams: no hue to learn */
        hues[nh++] = h;
        if(s < smin) smin = s;
        if(s > smax) smax = s;
        if(l < lmin) lmin = l;
        if(l > lmax) lmax = l;
    }
    if(nh == 0) { hues[nh++] = 30; smin = 0.35; smax = 0.75; lmin = 0.35; lmax = 0.75; }
    if(lmin < 0.25) lmin = 0.25;
    if(lmax > 0.82) lmax = 0.82;
    if(lmax < lmin) lmax = lmin;

    int n = 0;
    for(int tries = 0; n < GEN_COUNT && tries < GEN_COUNT * 20; tries++) {
        double h = rndf(r) < 0.8 ? hues[rnd(r) % (uint32_t)nh] + rndr(r, -28, 28) : rndr(r, 0, 360);
        uint32_t bg = from_hsl(h, rndr(r, smin, smax), rndr(r, lmin, lmax));
        bool dup = false;
        for(int i = 0; i < n; i++) if(distance(out[i].bg, bg) < NEAR_SAME) { dup = true; break; }
        if(dup) continue;
        out[n].bg = bg;                    /* inked like mix: from the palette, cream, off-black */
        collect_inks(&out[n], -1);
        n++;
    }
    return n;
}

/* ----------------------------------------------------------- dealing */

/* A deck of `count` cards, reshuffled each time through. take() hands out
 * the first card that `ok` accepts (searching the rest of the deck), and
 * swaps it to the front so the skipped ones come up next. */
typedef struct { int * cards; int count, pos; rng_t * r; } deck_t;

static void deck_init(deck_t * d, int count, rng_t * r)
{
    d->cards = malloc(sizeof(int) * (size_t)(count > 0 ? count : 1));
    d->count = d->cards ? count : 0;
    d->pos = 0;
    d->r = r;
    for(int i = 0; i < d->count; i++) d->cards[i] = i;
    shuffle(d->cards, d->count, r);
}

static int deck_take(deck_t * d, bool (*ok)(int card, const void * ctx), const void * ctx)
{
    if(d->count == 0) return 0;
    if(d->pos >= d->count) { shuffle(d->cards, d->count, d->r); d->pos = 0; }
    int pick = d->pos;
    for(int k = 0; k < d->count; k++) {
        int idx = d->pos + k;
        if(idx >= d->count) break;                  /* only this pass's remaining cards */
        if(ok(d->cards[idx], ctx)) { pick = idx; break; }
    }
    int t = d->cards[d->pos]; d->cards[d->pos] = d->cards[pick]; d->cards[pick] = t;
    return d->cards[d->pos++];
}

typedef struct { const uint32_t * bgs; uint32_t left, up; bool has_left, has_up; } colour_ctx_t;

static bool colour_ok(int card, const void * vctx)
{
    const colour_ctx_t * c = vctx;
    uint32_t bg = c->bgs[card];
    if(c->has_left && distance(bg, c->left) < TOO_CLOSE) return false;
    if(c->has_up && distance(bg, c->up) < TOO_CLOSE) return false;
    return true;
}

/* The font deck holds `share` cards per face; card -> face through face_of. */
static int face_of[FONT_MAX * 100];

typedef struct { int left, up; int letters; uint32_t bg, ink; bool want_long; } font_ctx_t;

static bool font_ok(int card, const void * vctx)
{
    const font_ctx_t * c = vctx;
    int i = face_of[card];
    if(i == c->left || i == c->up) return false;
    if(!tilestyle_font_fits(i, c->letters, c->bg, c->ink)) return false;
    if(c->want_long && !fonts[i].for_long) return false;
    return true;
}

void tilestyle_deal(unsigned seed, int n, int cols, const bool * noart, const int * letters, tile_style_t * out)
{
    if(n <= 0 || !out) return;
    memset(out, 0, sizeof(tile_style_t) * (size_t)n);
    for(int e = 0; e < n; e++) out[e].font = TILE_FONT_NONE;
    if(cols < 1) cols = 1;
    rng_t r = { seed * 2654435761u + 0x51ED270Bu };

    /* the pool for this visit: backgrounds, and how to find an ink */
    static mixbg_t mix[PAL_MAX > GEN_COUNT ? PAL_MAX : GEN_COUNT];
    static uint32_t bgs[PAIR_MAX];
    int pool = 0;
    tile_colours_t used = mode;
    if(mode == TILE_COLOURS_PALETTES) { pool = build_palettes(mix); for(int i = 0; i < pool; i++) bgs[i] = mix[i].bg; }
    else if(mode == TILE_COLOURS_MIX) { pool = build_mix(mix); for(int i = 0; i < pool; i++) bgs[i] = mix[i].bg; }
    else if(mode == TILE_COLOURS_GENERATE) { pool = build_generated(mix, &r); for(int i = 0; i < pool; i++) bgs[i] = mix[i].bg; }
    if(pool == 0) {                                   /* theme mode, or the others found nothing */
        used = TILE_COLOURS_THEME;
        pool = ntheme;
        for(int i = 0; i < pool; i++) bgs[i] = theme_pairs[i].bg;
    }

    deck_t cd, fd;
    deck_init(&cd, pool, &r);
    int cards = 0;
    for(int i = 0; i < nfonts; i++) for(int k = 0; k < fonts[i].share && cards < (int)(sizeof(face_of) / sizeof(face_of[0])); k++) face_of[cards++] = i;
    bool any_long = false;
    for(int i = 0; i < nfonts; i++) if(fonts[i].for_long) any_long = true;
    deck_init(&fd, cards, &r);

    for(int e = 0; e < n; e++) {
        if(!noart[e]) continue;
        int l = (e % cols > 0 && noart[e - 1]) ? e - 1 : -1;
        int u = (e >= cols && noart[e - cols]) ? e - cols : -1;
        if(pool > 0) {
            colour_ctx_t cc = { bgs, l >= 0 ? out[l].bg : 0, u >= 0 ? out[u].bg : 0, l >= 0, u >= 0 };
            int card = deck_take(&cd, colour_ok, &cc);
            out[e].bg = bgs[card];
            if(used == TILE_COLOURS_THEME) out[e].ink = theme_pairs[card].ink;
            else out[e].ink = pick_ink(&mix[card], &r);
        }
        else { out[e].bg = 0x333333; out[e].ink = 0xEEEEEE; }
        if(cards > 0) {
            int let = letters ? letters[e] : 0;
            /* a long title goes to a condensed face half the time (Ian:
             * "prioritize Anton SC for long titles") */
            bool want_long = any_long && let >= LONG_LETTERS && (rnd(&r) & 1);
            font_ctx_t fc = { l >= 0 ? out[l].font : -1, u >= 0 ? out[u].font : -1, let, out[e].bg, out[e].ink, want_long };
            int card = deck_take(&fd, font_ok, &fc);
            /* deck_take falls back to the next card when nothing passes; make
             * sure what we hand out at least fits the title and colours */
            int face = face_of[card];
            if(!tilestyle_font_fits(face, let, out[e].bg, out[e].ink)) {
                face = TILE_FONT_NONE;
                for(int k = 0; k < nfonts; k++) { int j = (face_of[card] + 1 + k) % nfonts; if(tilestyle_font_fits(j, let, out[e].bg, out[e].ink)) { face = j; break; } }
            }
            out[e].font = (uint8_t)face;
        }
    }
    free(cd.cards);
    free(fd.cards);
}

/* ----------------------------------------------------------- the album page */

void tilestyle_companion(unsigned seed, const tile_style_t * tile, tile_style_t * out)
{
    *out = *tile;
    int set = -1;
    for(int j = 0; j < nset_col; j++) if(set_col[j] == tile->bg) { set = set_of[j]; break; }
    rng_t r = { seed * 2654435761u + 0x27D4EB2Fu };
    static mixbg_t cand[PAL_MAX];
    int n = 0;
    for(int j = 0; j < nset_col; j++) {
        if(set_of[j] != set || is_dark(set_col[j]) || distance(set_col[j], tile->bg) < TOO_CLOSE) continue;
        cand[n].bg = set_col[j];
        collect_inks(&cand[n], set);
        if(cand[n].ncol == 0 && cand[n].ndark == 0 && !cand[n].cream_ok) continue;
        n++;
    }
    if(n > 0) {
        mixbg_t * m = &cand[rnd(&r) % (uint32_t)n];
        out->bg = m->bg;
        out->ink = pick_ink(m, &r);
        return;
    }
    /* inverted: the ink as the page when it can host the tile's colour */
    if(!is_dark(tile->ink) || reads(tile->ink, tile->bg)) { out->bg = tile->ink; out->ink = tile->bg; }
}

int tilestyle_set_colours(uint32_t bg, uint32_t * out, int max)
{
    int set = -1;
    for(int j = 0; j < nset_col; j++) if(set_col[j] == bg) { set = set_of[j]; break; }
    if(set < 0) return 0;
    int n = 0;
    for(int j = 0; j < nset_col && n < max; j++) if(set_of[j] == set && set_col[j] != bg) out[n++] = set_col[j];
    return n;
}

void tilestyle_picture_colours(unsigned seed, const tile_style_t * tile, uint32_t * label, uint32_t * shell, uint32_t * dark)
{
    uint32_t set[PAL_MAX];
    int n = tilestyle_set_colours(tile->bg, set, PAL_MAX);
    rng_t r = { seed * 2654435761u + 0x165667B1u };

    /* the label: a set colour the ink reads on, the most colourful of them
     * (a label apart from the background is worth a little saturation) */
    *label = tile->bg;
    double best = -1;
    for(int i = 0; i < n; i++) {
        uint32_t c = set[i];
        if(!reads(c, tile->ink)) continue;
        double score = tilestyle_saturation(c) + (distance(c, tile->bg) >= TOO_CLOSE ? 0.5 : 0);
        if(score > best) { best = score; *label = c; }
    }

    /* the shell: another colour of the set, apart from the background and
     * the label, dealt; the ink itself when the set has nothing else (a
     * black or cream shell round a coloured label) */
    uint32_t cand[PAL_MAX];
    int nc = 0;
    for(int i = 0; i < n; i++) {
        uint32_t c = set[i];
        if(c == *label || distance(c, tile->bg) < TOO_CLOSE || distance(c, *label) < NEAR_SAME) continue;
        cand[nc++] = c;
    }
    if(nc > 0) *shell = cand[rnd(&r) % (uint32_t)nc];
    else if(distance(tile->ink, *label) >= NEAR_SAME) *shell = tile->ink;
    else *shell = own_ink(tile->bg);

    /* the darkest of the set (a record's disc), the theme's off-black when
     * the set's darkest is the background itself or too near it */
    *dark = 0;
    double lum = 2;
    for(int i = 0; i < n; i++) if(luminance(set[i]) < lum && distance(set[i], tile->bg) >= TOO_CLOSE) { lum = luminance(set[i]); *dark = set[i]; }
    if(!*dark || lum > 0.1) *dark = tilestyle_dark_ink();
}

const page_layout_t * tilestyle_layout_pick(unsigned seed, page_screen_t screen)
{
    int s = screen == PAGE_SCREEN_TV ? 0 : 1;
    if(ldeck_pos[s] >= ldeck_n[s]) {             /* a fresh deck: every layout once before any repeats */
        rng_t r = { seed * 2654435761u + 0x7F4A7C15u };
        int n = 0;
        for(int i = 0; i < nlayouts; i++) if(layouts[i].screen == screen) ldeck[s][n++] = i;
        ldeck_n[s] = n;
        ldeck_pos[s] = 0;
        shuffle(ldeck[s], n, &r);
    }
    if(ldeck_n[s] == 0) return NULL;
    return &layouts[ldeck[s][ldeck_pos[s]++]];
}

int tilestyle_liner_pick(unsigned seed)
{
    if(nliners == 0 || liner_share <= 0) return -1;
    rng_t r = { seed * 2654435761u + 0x3C6EF372u };
    int x = (int)(rnd(&r) % (uint32_t)liner_share);
    for(int i = 0; i < nliners; i++) { x -= liners[i].share; if(x < 0) return i; }
    return nliners - 1;
}

int tilestyle_hand_count(void) { return nhands; }
const mix_hand_t * tilestyle_hand(int i) { return i >= 0 && i < nhands ? &hands[i] : NULL; }
const mix_hand_t * tilestyle_hand_for(const char * key)
{
    if(nhands == 0) return NULL;
    unsigned h = 2166136261u;
    for(const unsigned char * p = (const unsigned char *)(key ? key : ""); *p; p++) h = (h ^ *p) * 16777619u;   /* FNV-1a: the same key, the same hand, always */
    return &hands[h % (unsigned)nhands];
}

void tilestyle_page_accents(unsigned seed, uint32_t bg, uint32_t ink, uint32_t * play, uint32_t * sel)
{
    typedef struct { uint32_t c; double s; } cand_t;
    cand_t cand[PAL_MAX + 1];
    int n = 0;
    double floor = high_contrast ? list_contrast : min_contrast;   /* the theme's own floor: vibe over contrast; high contrast: the list floor */
    for(int i = 0; i <= npal; i++) {
        uint32_t c = i < npal ? pal[i] : accent;
        if(c == 0 && i == npal) break;
        if(tilestyle_contrast(bg, c) < floor || distance(bg, c) < TOO_CLOSE || distance(ink, c) < TOO_CLOSE) continue;
        double h, s, l;
        to_hsl(c, &h, &s, &l);
        if(l < 0.12 || l > 0.93) continue;                      /* near-blacks and creams aren't accents */
        bool dup = false;
        for(int k = 0; k < n; k++) if(distance(cand[k].c, c) < NEAR_SAME) { dup = true; break; }
        if(dup) continue;
        cand[n].c = c; cand[n].s = s; n++;
    }
    for(int i = 1; i < n; i++) {                                 /* most saturated first */
        cand_t v = cand[i]; int j = i - 1;
        while(j >= 0 && cand[j].s < v.s) { cand[j + 1] = cand[j]; j--; }
        cand[j + 1] = v;
    }
    if(n == 0) { *play = *sel = ink; return; }
    rng_t r = { seed * 2654435761u + 0x1B873593u };
    int top = n < 3 ? n : 3;
    int p = (int)(rnd(&r) % (uint32_t)top);
    *play = cand[p].c;
    *sel = ink;
    for(int i = 0; i < n; i++) {
        if(i == p || distance(cand[i].c, *play) < TOO_CLOSE) continue;
        *sel = cand[i].c;
        break;
    }
}

void tilestyle_prefer_track(const char * uri, const tile_style_t * tile, unsigned seed)
{
    if(!uri || !tile) return;
    uint32_t label, shell, dark;
    tilestyle_picture_colours(seed, tile, &label, &shell, &dark);
    unsigned x = seed ^ 0x9E3779B9u;
    for(const char * p = uri; *p; p++) x = (x ^ (unsigned char)*p) * 16777619u;
    bool black = x % 5 == 0;                            /* coloured vinyl most times, black one in five */
    uint32_t disc = black ? dark : shell;
    placeholder_tint_t tints[8] = { {"background", tile->bg}, {"label", label}, {"shell", shell}, {"sleeve", shell},
                                    {"centre", label}, {"disc", disc}, {"groove", disc}, {"spindle", label} };
    const tile_font_t * f = tile->font != TILE_FONT_NONE ? tilestyle_font(tile->font) : NULL;
    placeholder_prefer_track(uri, tints, 8, tile->bg, tile->ink, f ? f->file : NULL, f ? f->caps : false);
}
