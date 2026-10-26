/*
 * SimpletonOS UI - the liner sheet (implementation). See liner.h.
 */
#include "liner.h"
#include "curation.h"
#include "layout.h"
#include "mpdc.h"
#include "palette.h"
#include "playthrough.h"
#include "textfit.h"
#include "theme.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RULES      14
#define SLOTS      7
#define TAIL_PCT   70                 /* a bracket tail's size, % of the title's */
#define SHRINK_PCT 72                 /* how small a title may go before it is cut */
#define GROW_PCT   112                /* the selected title grows a little, as rows do elsewhere */

struct liner_sheet {
    lv_obj_t * obj, * rules[RULES], * margin_a, * margin_b;
    lv_obj_t * l_artist[SLOTS], * l_title[SLOTS], * l_tail[SLOTS], * l_num[SLOTS], * l_mark[SLOTS];
    int x, y, w, h;
    uint32_t paper, ink;              /* the paper, the colour for the selected (or playing) track */
    char face[64];
    hand_case_t letter_case;
    int size_pct;
    bool handwritten, numbers;
    unsigned seed;
};

/* ------------------------------------------------------------ the paper */

static unsigned rnd(liner_sheet_t * s) { s->seed = s->seed * 1103515245u + 12345u; return s->seed >> 8; }

/* saturation 0..1 of RRGGBB */
static double sat(uint32_t c)
{
    int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    return mx == 0 ? 0 : (double)(mx - mn) / mx;
}

/* The paper: a pastel from a palette set (nothing deep in any channel),
 * multiplied over white at 80 %; and the colour for the selected track
 * from the same set - the most colourful one that reads at 2.5:1 or the
 * theme's floor if higher, else the set's darkest, else the dark ink. */
static void deal_paper(liner_sheet_t * s)
{
    int nsets = tilestyle_set_count();
    uint32_t cand[64]; int cand_set[64]; int n = 0;
    for(int k = 0; k < nsets && n < 64; k++) {
        uint32_t cs[8]; int m = tilestyle_set(k, cs, 8);
        for(int i = 0; i < m && n < 64; i++) {
            uint32_t c = cs[i]; unsigned mn = (c >> 16) & 255; if(((c >> 8) & 255) < mn) mn = (c >> 8) & 255; if((c & 255) < mn) mn = c & 255;
            if(mn >= 0x80 && tilestyle_contrast(c, 0x101010) >= 8.0) { cand[n] = c; cand_set[n] = k; n++; }
        }
    }
    uint32_t raw = 0xFAEFD0; int set = -1;
    if(n) { int k = (int)(rnd(s) % (unsigned)n); raw = cand[k]; set = cand_set[k]; }
    s->paper = ui_mix(raw, 0xFFFFFF, 20);
    double floor_c = tilestyle_min_contrast() > 2.5 ? tilestyle_min_contrast() : 2.5;
    uint32_t best = 0, dark = 0x24281B; double best_sat = -1, dark_lum = 99;
    uint32_t cs[8]; int m = set >= 0 ? tilestyle_set(set, cs, 8) : 0;
    for(int i = 0; i < m; i++) {
        if(cs[i] == raw) continue;
        double c = tilestyle_contrast(cs[i], s->paper);
        if(c >= floor_c && sat(cs[i]) > best_sat) { best = cs[i]; best_sat = sat(cs[i]); }
        double lum = tilestyle_contrast(cs[i], 0xFFFFFF);
        if(lum < dark_lum) { dark_lum = lum; dark = cs[i]; }
    }
    s->ink = best ? best : (tilestyle_contrast(dark, s->paper) >= floor_c ? dark : tilestyle_dark_ink());
    if(tilestyle_contrast(s->ink, s->paper) < 2.5) s->ink = tilestyle_dark_ink();
}

static int rule_y(const liner_sheet_t * s, int i) { return (int)((long)i * s->h * 2 / (2 * RULES + 1)); }   /* rule i of 1..14; half a rule of air at the top */
static int margin_x(const liner_sheet_t * s) { return s->w * 115 / 1000; }

/* ------------------------------------------------------------ the text */

static void cased(char * t, hand_case_t c)
{
    if(c == HAND_UPPER) { char u[512]; ui_caps(t, u, sizeof(u)); snprintf(t, 256, "%.255s", u); }
    else if(c == HAND_LOWER) for(char * p = t; *p; p++) *p = (char)tolower((unsigned char)*p);
}

/* "Title (tail)" -> head, tail (the tail keeps its brackets) */
static void split_tail(const char * title, char * head, size_t hl, char * tail, size_t tl)
{
    const char * p = title; const char * at = NULL;
    for(; *p; p++) if((*p == '(' || *p == '[') && p > title && p[-1] == ' ') { at = p; break; }
    if(!at) { snprintf(head, hl, "%s", title); tail[0] = 0; return; }
    size_t n = (size_t)(at - title); while(n && title[n - 1] == ' ') n--;
    snprintf(head, hl, "%.*s", (int)n, title); snprintf(tail, tl, "%s", at);
}

/* put a label's baseline on a rule */
static void on_rule(const liner_sheet_t * s, lv_obj_t * l, const lv_font_t * f, int x, int rule, bool right)
{
    int lh = lv_font_get_line_height(f);
    int y = rule_y(s, rule) - lh + f->base_line - PX(3);
    lv_obj_set_style_text_font(l, f, 0);
    if(right) lv_obj_align(l, LV_ALIGN_TOP_RIGHT, x - s->w, y); else lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, y);
}

static void cut_dots(char * t, const lv_font_t * f, int maxw)
{
    size_t n = strlen(t);
    char u[300];
    while(n > 0) {
        snprintf(u, sizeof(u), "%.*s…", (int)n, t);
        if(text_w(f, u) <= maxw) break;
        n--; while(n > 0 && ((unsigned char)t[n] & 0xC0) == 0x80) n--;
    }
    snprintf(t, 256, "%.255s", n ? u : "…");
}

static void clear_slot(liner_sheet_t * s, int k)
{
    lv_obj_add_flag(s->l_artist[k], LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(s->l_title[k], LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(s->l_tail[k], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s->l_num[k], LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(s->l_mark[k], LV_OBJ_FLAG_HIDDEN);
}

static void write_slot(liner_sheet_t * s, int k, const liner_row_t * r, int number, bool coloured, bool grown, bool is_playing)
{
    int x = margin_x(s) + s->w * 28 / 1000, right = s->w - s->w * 4 / 100, maxw = right - x;
    int ua = 2 * k + 1, ut = 2 * k + 2;                                 /* the two rules */
    uint32_t ink = coloured ? s->ink : 0x0A0A0A, ink_a = coloured ? s->ink : 0x202020;
    int size_a = PX(24) * s->size_pct / 100, size_t_ = PX(38) * s->size_pct / 100 * (grown ? GROW_PCT : 100) / 100, size_n = PX(24) * s->size_pct / 100;
    char artist[256], head[256], tail[256];
    snprintf(artist, sizeof(artist), "%.255s", r->artist ? r->artist : "");
    if(s->handwritten) cased(artist, s->letter_case); else { char t[512]; ui_caps(artist, t, sizeof(t)); snprintf(artist, sizeof(artist), "%.255s", t); }
    split_tail(r->title ? r->title : "", head, sizeof(head), tail, sizeof(tail));
    if(s->handwritten) { cased(head, s->letter_case); cased(tail, s->letter_case); }
    const lv_font_t * fa = ui_font_face_px(s->face, size_a, true);
    if(text_w(fa, artist) > maxw) cut_dots(artist, fa, maxw);
    lv_label_set_text(s->l_artist[k], artist); lv_obj_set_style_text_color(s->l_artist[k], lv_color_hex(ink_a), 0);
    on_rule(s, s->l_artist[k], fa, x, ua, false);
    /* the title: shrink to fit, then the tail to dots, then the head */
    int sz = size_t_; const lv_font_t * fh, * ft; int w;
    for(;;) {
        fh = ui_font_face_px(s->face, sz, true); ft = ui_font_face_px(s->face, sz * TAIL_PCT / 100, true);
        w = text_w(fh, head) + (tail[0] ? text_w(ft, " ") + text_w(ft, tail) : 0);
        if(w <= maxw || sz <= size_t_ * SHRINK_PCT / 100) break;
        sz -= 2;
    }
    if(w > maxw && tail[0]) { int room = maxw - text_w(fh, head) - text_w(ft, " "); if(room > text_w(ft, "(…")) cut_dots(tail, ft, room); else tail[0] = 0; }
    if(text_w(fh, head) > maxw) { cut_dots(head, fh, maxw); tail[0] = 0; }
    lv_label_set_text(s->l_title[k], head); lv_obj_set_style_text_color(s->l_title[k], lv_color_hex(ink), 0);
    on_rule(s, s->l_title[k], fh, x, ut, false);
    if(tail[0]) {
        char t[300]; snprintf(t, sizeof(t), " %s", tail);
        lv_label_set_text(s->l_tail[k], t); lv_obj_set_style_text_color(s->l_tail[k], lv_color_hex(ink), 0);
        on_rule(s, s->l_tail[k], ft, x + text_w(fh, head), ut, false);
        lv_obj_remove_flag(s->l_tail[k], LV_OBJ_FLAG_HIDDEN);
    }
    else lv_obj_add_flag(s->l_tail[k], LV_OBJ_FLAG_HIDDEN);
    /* the margin: the number, the triangle */
    const lv_font_t * fn = ui_font_face_px(s->face, size_n, true);
    int nx = margin_x(s) - s->w * 26 / 1000;
    if(s->numbers) {
        char n[16]; snprintf(n, sizeof(n), "%d", number);
        lv_label_set_text(s->l_num[k], n); lv_obj_set_style_text_color(s->l_num[k], lv_color_hex(coloured ? s->ink : 0x282828), 0);
        on_rule(s, s->l_num[k], fn, nx, ut, true);
        lv_obj_remove_flag(s->l_num[k], LV_OBJ_FLAG_HIDDEN);
        nx -= text_w(fn, n) + s->w * 16 / 1000;
    }
    else lv_obj_add_flag(s->l_num[k], LV_OBJ_FLAG_HIDDEN);
    if(is_playing) {
        const lv_font_t * fm = ui_font_px(size_n * 3 / 4);
        lv_label_set_text(s->l_mark[k], LV_SYMBOL_PLAY); lv_obj_set_style_text_color(s->l_mark[k], lv_color_hex(coloured ? s->ink : 0x282828), 0);
        on_rule(s, s->l_mark[k], fm, nx, ut, true);
        lv_obj_remove_flag(s->l_mark[k], LV_OBJ_FLAG_HIDDEN);
    }
    else lv_obj_add_flag(s->l_mark[k], LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s->l_artist[k], LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(s->l_title[k], LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------ the sheet */

liner_sheet_t * liner_sheet_create(lv_obj_t * parent)
{
    liner_sheet_t * s = calloc(1, sizeof(*s));
    if(!s) return NULL;
    s->obj = lv_obj_create(parent);
    lv_obj_remove_style_all(s->obj);
    lv_obj_set_style_bg_opa(s->obj, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s->obj, LV_OBJ_FLAG_SCROLLABLE);
    for(int i = 0; i < RULES; i++) { s->rules[i] = lv_obj_create(s->obj); lv_obj_remove_style_all(s->rules[i]); lv_obj_set_style_bg_opa(s->rules[i], LV_OPA_COVER, 0); }
    s->margin_a = lv_obj_create(s->obj); lv_obj_remove_style_all(s->margin_a); lv_obj_set_style_bg_opa(s->margin_a, LV_OPA_COVER, 0);
    s->margin_b = lv_obj_create(s->obj); lv_obj_remove_style_all(s->margin_b); lv_obj_set_style_bg_opa(s->margin_b, LV_OPA_COVER, 0);
    for(int k = 0; k < SLOTS; k++) {
        s->l_artist[k] = lv_label_create(s->obj); s->l_title[k] = lv_label_create(s->obj); s->l_tail[k] = lv_label_create(s->obj);
        s->l_num[k] = lv_label_create(s->obj); s->l_mark[k] = lv_label_create(s->obj);
        clear_slot(s, k);
    }
    snprintf(s->face, sizeof(s->face), "Jost-Medium.ttf"); s->size_pct = 100; s->paper = 0xFAEFD0; s->ink = 0x24281B;
    lv_obj_add_flag(s->obj, LV_OBJ_FLAG_HIDDEN);
    return s;
}

void liner_sheet_delete(liner_sheet_t * s)
{
    if(!s) return;
    lv_obj_delete(s->obj);
    free(s);
}

void liner_sheet_place(liner_sheet_t * s, int x, int y, int w, int h)
{
    if(!s) return;
    s->x = x; s->y = y; s->w = w; s->h = h;
    lv_obj_set_pos(s->obj, x, y); lv_obj_set_size(s->obj, w, h);
    int lw = PX(2) > 1 ? PX(2) : 2, mx = margin_x(s);
    for(int i = 0; i < RULES; i++) { lv_obj_set_size(s->rules[i], w, lw); lv_obj_set_pos(s->rules[i], 0, rule_y(s, i + 1)); }
    lv_obj_set_size(s->margin_a, lw, h); lv_obj_set_pos(s->margin_a, mx - w * 11 / 1000, 0);
    lv_obj_set_size(s->margin_b, lw, h); lv_obj_set_pos(s->margin_b, mx + w * 3 / 1000, 0);
}

void liner_sheet_deal(liner_sheet_t * s, unsigned seed, const char * hand, hand_case_t letter_case, int size_pct)
{
    if(!s) return;
    s->seed = seed * 2654435761u + 7u;
    deal_paper(s);
    s->numbers = rnd(s) % 2 == 0;                                       /* numbered on some sheets, not others (Ian) */
    if(hand && hand[0]) { snprintf(s->face, sizeof(s->face), "%.63s", hand); s->letter_case = letter_case; s->size_pct = size_pct > 0 ? size_pct : 100; s->handwritten = true; }
    else {
        int li = tilestyle_liner_pick(rnd(s));
        const tile_font_t * f = li >= 0 ? tilestyle_liner(li) : NULL;
        snprintf(s->face, sizeof(s->face), "%.63s", f ? f->file : "Jost-Medium.ttf");
        s->letter_case = HAND_MIXED; s->size_pct = 100; s->handwritten = false;
    }
    lv_obj_set_style_bg_color(s->obj, lv_color_hex(s->paper), 0);
    uint32_t rule_c = ui_mix(s->paper, 0x8C96A5, 70), margin_c = ui_mix(s->paper, 0xB8646C, 70);
    for(int i = 0; i < RULES; i++) lv_obj_set_style_bg_color(s->rules[i], lv_color_hex(rule_c), 0);
    lv_obj_set_style_bg_color(s->margin_a, lv_color_hex(margin_c), 0);
    lv_obj_set_style_bg_color(s->margin_b, lv_color_hex(margin_c), 0);
    fprintf(stderr, "simpleton-ui: liner: paper %06X, ink %06X, %s%s, %s\n", s->paper, s->ink, s->face, s->handwritten ? " (the mixtape's hand)" : "", s->numbers ? "numbered" : "no numbers");
}

void liner_sheet_write(liner_sheet_t * s, const liner_row_t * rows, int n, int first, int selected, int playing)
{
    if(!s) return;
    for(int k = 0; k < SLOTS; k++) {
        int i = first + k;
        if(i >= 0 && i < n) write_slot(s, k, &rows[i], i + 1, selected >= 0 ? i == selected : i == playing, selected >= 0 && i == selected, i == playing);
        else clear_slot(s, k);
    }
}

void liner_sheet_show(liner_sheet_t * s, bool on)
{
    if(!s) return;
    if(on) { lv_obj_remove_flag(s->obj, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(s->obj); }
    else lv_obj_add_flag(s->obj, LV_OBJ_FLAG_HIDDEN);
}

uint32_t liner_sheet_paper(const liner_sheet_t * s) { return s ? s->paper : 0xFAEFD0; }
uint32_t liner_sheet_ink(const liner_sheet_t * s) { return s ? s->ink : 0x24281B; }

/* ------------------------------------------------------------ the saver */

#define QUEUE_MAX 600
typedef struct { char title[256], artist[256]; } qrow_t;
static liner_sheet_t * saver_sheet;
static qrow_t * queue; static liner_row_t * qrows; static int queue_n;
static bool active;
static int page = -1, playing = -1, last_len = -1;

static bool queue_cb(const mpd_song_info_t * song, void * ctx)
{
    (void)ctx;
    if(queue_n >= QUEUE_MAX) return false;
    qrow_t * r = &queue[queue_n];
    const char * slash = strrchr(song->uri, '/');
    snprintf(r->title, sizeof(r->title), "%.255s", song->title[0] ? song->title : slash ? slash + 1 : song->uri);
    snprintf(r->artist, sizeof(r->artist), "%.255s", song->artist);
    qrows[queue_n].title = r->title; qrows[queue_n].artist = r->artist;
    queue_n++;
    return true;
}

void liner_create(lv_obj_t * parent)
{
    saver_sheet = liner_sheet_create(parent);
    queue = calloc(QUEUE_MAX, sizeof(qrow_t));
    qrows = calloc(QUEUE_MAX, sizeof(liner_row_t));
}

void liner_start(int x, int w)
{
    if(!saver_sheet || !queue || !qrows) return;
    active = true;
    liner_sheet_place(saver_sheet, x, 0, w, ui_screen.h);
    const mix_hand_t * h = NULL;
    int uid = playthrough_playing_mixtape();
    if(uid) { char key[96] = ""; curation_key(uid, key, sizeof(key)); h = tilestyle_hand_for(key); }
    unsigned seed = (unsigned)lv_tick_get() ^ ((unsigned)time(NULL) * 2654435761u);
    liner_sheet_deal(saver_sheet, seed, h ? h->file : NULL, h ? h->letter_case : HAND_MIXED, h ? h->size_pct : 100);
    page = -1; playing = -1; last_len = -1;
    liner_mpd_changed();
    liner_sheet_show(saver_sheet, true);
}

void liner_stop(void)
{
    if(!active) return;
    active = false;
    liner_sheet_show(saver_sheet, false);
}

bool liner_active(void) { return active; }

void liner_mpd_changed(void)
{
    if(!active) return;
    mpd_status_t st;
    if(!mpd_status(&st)) return;
    int pos = st.songid >= 0 ? st.song : -1;
    if(pos == playing && st.playlistlength == last_len && page >= 0) return;   /* polled every 2 s: nothing moved */
    last_len = st.playlistlength;
    queue_n = 0;
    mpd_queue(queue_cb, NULL);
    page = pos >= 0 ? pos / SLOTS : 0; playing = pos;
    liner_sheet_write(saver_sheet, qrows, queue_n, page * SLOTS, -1, playing);
}
