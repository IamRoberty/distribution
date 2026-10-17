/*
 * SimpletonOS UI - the small on-unit keyboard (implementation). See keyboard.h.
 */
#include "keyboard.h"
#include "layout.h"
#include "strings.h"
#include "textfit.h"
#include "theme.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ROWS   4
#define MAX_COLS   12
#define CTRL_ROW   MAX_ROWS                /* the control row's index */
#define NCTRL      5
#define MAX_PAGES  4

/* A page: up to four rows of keys, space-separated UTF-8; `upper` the same
 * keys shifted (NULL rows = no capitals on that row). */
typedef struct { const char * name; const char * lower[MAX_ROWS]; const char * upper[MAX_ROWS]; } page_t;

static const page_t PAGE_LATIN = { "ABC",
    { "1 2 3 4 5 6 7 8 9 0", "q w e r t y u i o p", "a s d f g h j k l '", "z x c v b n m , . -" },
    { "1 2 3 4 5 6 7 8 9 0", "Q W E R T Y U I O P", "A S D F G H J K L \"", "Z X C V B N M ; : _" } };
static const page_t PAGE_SYMBOLS = { "#+=",
    { "1 2 3 4 5 6 7 8 9 0", "- _ : ; ( ) [ ] & @", "! ? \" ' + = / * # %", "~ ^ ° € £ $ · … — ♥" },
    { NULL, NULL, NULL, NULL } };
static const page_t PAGE_RUSSIAN = { "АБВ",
    { "й ц у к е н г ш щ з х ъ", "ф ы в а п р о л д ж э ё", "я ч с м и т ь б ю , . -", NULL },
    { "Й Ц У К Е Н Г Ш Щ З Х Ъ", "Ф Ы В А П Р О Л Д Ж Э Ё", "Я Ч С М И Т Ь Б Ю ; : _", NULL } };
static const page_t PAGE_UKRAINIAN = { "АБВ",
    { "й ц у к е н г ш щ з х ї", "ф і в а п р о л д ж є ґ", "я ч с м и т ь б ю ' . -", NULL },
    { "Й Ц У К Е Н Г Ш Щ З Х Ї", "Ф І В А П Р О Л Д Ж Є Ґ", "Я Ч С М И Т Ь Б Ю \" : _", NULL } };
static const page_t PAGE_SERBIAN = { "АБВ",
    { "љ њ е р т з у и о п ш ђ", "а с д ф г х ј к л ч ћ ж", "џ ц в б н м , . - ' ! ?", NULL },
    { "Љ Њ Е Р Т З У И О П Ш Ђ", "А С Д Ф Г Х Ј К Л Ч Ћ Ж", "Џ Ц В Б Н М ; : _ \" ! ?", NULL } };
static const page_t PAGE_GREEK = { "ΑΒΓ",
    { "ς ε ρ τ υ θ ι ο π ά έ ή", "α σ δ φ γ η ξ κ λ ί ό ύ", "ζ χ ψ ω β ν μ ώ ϊ ϋ , .", NULL },
    { "Σ Ε Ρ Τ Υ Θ Ι Ο Π Ά Έ Ή", "Α Σ Δ Φ Γ Η Ξ Κ Λ Ί Ό Ύ", "Ζ Χ Ψ Ω Β Ν Μ Ώ Ϊ Ϋ ; :", NULL } };

/* The letters a Latin-script language adds to ABC: one page of them. */
typedef struct { const char * lang; const char * lower; const char * upper; } accents_t;
static const accents_t ACCENTS[] = {
    { "fr", "à â æ ç é è ê ë î ï ô œ ù û ü ÿ « »", "À Â Æ Ç É È Ê Ë Î Ï Ô Œ Ù Û Ü Ÿ « »" },
    { "de", "ä ö ü ß „ “", "Ä Ö Ü ẞ „ “" },
    { "es", "á é í ñ ó ú ü ¿ ¡", "Á É Í Ñ Ó Ú Ü ¿ ¡" },
    { "pt", "á â ã à ç é ê í ó ô õ ú", "Á Â Ã À Ç É Ê Í Ó Ô Õ Ú" },
    { "it", "à è é ì ò ó ù", "À È É Ì Ò Ó Ù" },
    { "ca", "à ç è é í ï ò ó ú ü ·", "À Ç È É Í Ï Ò Ó Ú Ü ·" },
    { "nl", "é è ë ï ö ü", "É È Ë Ï Ö Ü" },
    { "pl", "ą ć ę ł ń ó ś ź ż", "Ą Ć Ę Ł Ń Ó Ś Ź Ż" },
    { "cs", "á č ď é ě í ň ó ř š ť ú ů ý ž", "Á Č Ď É Ě Í Ň Ó Ř Š Ť Ú Ů Ý Ž" },
    { "sk", "á ä č ď é í ĺ ľ ň ó ô ŕ š ť ú ý ž", "Á Ä Č Ď É Í Ĺ Ľ Ň Ó Ô Ŕ Š Ť Ú Ý Ž" },
    { "hu", "á é í ó ö ő ú ü ű", "Á É Í Ó Ö Ő Ú Ü Ű" },
    { "ro", "ă â î ș ț", "Ă Â Î Ș Ț" },
    { "hr", "č ć đ š ž", "Č Ć Đ Š Ž" },
    { "sl", "č š ž", "Č Š Ž" },
    { "bs", "č ć đ š ž", "Č Ć Đ Š Ž" },
    { "tr", "ç ğ ı i ö ş ü", "Ç Ğ I İ Ö Ş Ü" },
    { "sv", "å ä ö é", "Å Ä Ö É" },
    { "fi", "å ä ö", "Å Ä Ö" },
    { "da", "æ ø å é", "Æ Ø Å É" },
    { "no", "æ ø å é", "Æ Ø Å É" },
    { "nb", "æ ø å é", "Æ Ø Å É" },
    { "is", "á ð é í ó ú ý þ æ ö", "Á Ð É Í Ó Ú Ý Þ Æ Ö" },
    { "et", "ä ö õ ü š ž", "Ä Ö Õ Ü Š Ž" },
    { "lv", "ā č ē ģ ī ķ ļ ņ š ū ž", "Ā Č Ē Ģ Ī Ķ Ļ Ņ Š Ū Ž" },
    { "lt", "ą č ę ė į š ų ū ž", "Ą Č Ę Ė Į Š Ų Ū Ž" },
    { "vi", "ă â đ ê ô ơ ư", "Ă Â Đ Ê Ô Ơ Ư" },
    { "ga", "á é í ó ú", "Á É Í Ó Ú" },
    { "cy", "â ê î ô û ŵ ŷ", "Â Ê Î Ô Û Ŵ Ŷ" },
    { "eo", "ĉ ĝ ĥ ĵ ŝ ŭ", "Ĉ Ĝ Ĥ Ĵ Ŝ Ŭ" },
};
#define NACCENTS ((int)(sizeof(ACCENTS) / sizeof(ACCENTS[0])))

/* The accents page is built at open from the language's letters: split
 * into rows of at most ten. */
static page_t page_accents;
static char   acc_lower[MAX_ROWS][200], acc_upper[MAX_ROWS][200], acc_name[24];

static void build_accents(const accents_t * a)
{
    memset(&page_accents, 0, sizeof(page_accents));
    const char * src[2] = { a->lower, a->upper };
    char (*dst[2])[200] = { acc_lower, acc_upper };
    for(int k = 0; k < 2; k++) {
        int r = 0, n = 0;
        dst[k][0][0] = 0;
        const char * p = src[k];
        while(*p && r < MAX_ROWS) {
            while(*p == ' ') p++;
            if(!*p) break;
            const char * e = p;
            while(*e && *e != ' ') e++;
            if(n == 10) { r++; n = 0; if(r >= MAX_ROWS) break; dst[k][r][0] = 0; }
            size_t l = strlen(dst[k][r]);
            snprintf(dst[k][r] + l, 200 - l, "%s%.*s", n ? " " : "", (int)(e - p), p);
            n++;
            p = e;
        }
        for(int i = 0; i <= r && i < MAX_ROWS; i++) (k ? page_accents.upper : page_accents.lower)[i] = dst[k][i];
    }
    /* the key's name: the first three capitals */
    const char * u = a->upper;
    int letters = 0;
    size_t o = 0;
    while(*u && letters < 3 && o < sizeof(acc_name) - 5) {
        if(*u == ' ') { u++; continue; }
        do { acc_name[o++] = *u++; } while(*u && ((unsigned char)*u & 0xC0) == 0x80);
        letters++;
    }
    acc_name[o] = 0;
    page_accents.name = acc_name;
}

/* The pages for the UI's language (Ian, 15 Oct: English gets ABC and the
 * symbols only; another language brings its own alphabet, first, by
 * itself): Cyrillic / Greek languages: their alphabet, ABC, symbols;
 * a Latin language with letters of its own: ABC, its accents, symbols;
 * anything else: ABC, symbols (names in other scripts come from the
 * phone app). */
static const page_t * pages[MAX_PAGES];
static int npages;

static void choose_pages(void)
{
    const char * lang = strings_language();
    char base[8];
    size_t i = 0;
    while(lang[i] && lang[i] != '-' && lang[i] != '_' && i < sizeof(base) - 1) { base[i] = lang[i]; i++; }
    base[i] = 0;
    npages = 0;
    if(strcmp(base, "ru") == 0 || strcmp(base, "be") == 0 || strcmp(base, "bg") == 0 || strcmp(base, "kk") == 0 || strcmp(base, "ky") == 0 || strcmp(base, "mn") == 0) pages[npages++] = &PAGE_RUSSIAN;
    else if(strcmp(base, "uk") == 0) pages[npages++] = &PAGE_UKRAINIAN;
    else if(strcmp(base, "sr") == 0 || strcmp(base, "mk") == 0) pages[npages++] = &PAGE_SERBIAN;
    else if(strcmp(base, "el") == 0) pages[npages++] = &PAGE_GREEK;
    pages[npages++] = &PAGE_LATIN;
    for(int k = 0; k < NACCENTS; k++) if(strcmp(ACCENTS[k].lang, base) == 0) { build_accents(&ACCENTS[k]); pages[npages++] = &page_accents; break; }
    pages[npages++] = &PAGE_SYMBOLS;
}

static char   text[KEYBOARD_MAX + 1];
static char   title_text[120];
static int    page_i, row, col, shift;     /* shift: 0 off, 1 once, 2 locked */
static bool   active;
static void (*done_cb)(const char *, void *);
static void * done_ctx;

static lv_obj_t * sheet, * box, * title_l, * field, * keys[MAX_ROWS][MAX_COLS], * ctrl[NCTRL];
static int ncols[MAX_ROWS];
static char keytext[MAX_ROWS][MAX_COLS][8];

/* ------------------------------------------------------------ layout */

static int key_w(void) { return PX(52); }
static int key_h(void) { return PX(50); }
static int key_gap(void) { return PX(6); }

static void split_row(const char * src, int r)
{
    ncols[r] = 0;
    if(!src) return;
    const char * p = src;
    while(*p && ncols[r] < MAX_COLS) {
        while(*p == ' ') p++;
        if(!*p) break;
        int n = 0;
        while(*p && *p != ' ' && n < 7) keytext[r][ncols[r]][n++] = *p++;
        while(*p && *p != ' ') p++;
        keytext[r][ncols[r]][n] = 0;
        ncols[r]++;
    }
}

static const char * ctrl_text(int i)
{
    switch(i) {
        case 0: return T(shift == 2 ? S_KB_SHIFT_LOCK : S_KB_SHIFT);
        case 1: return pages[(page_i + 1) % npages]->name;
        case 2: return T(S_KB_SPACE);
        case 3: return T(S_KB_DELETE);
        default: return T(S_KB_DONE);
    }
}

static lv_obj_t * key_obj(int r, int c) { return r == CTRL_ROW ? (c < NCTRL ? ctrl[c] : NULL) : (c < ncols[r] ? keys[r][c] : NULL); }

/* One key's focus look (0.21b: moving touches two keys, nothing else -
 * the whole box was being redrawn on every press, which made the D-pad
 * crawl and the TV remote's repeats pile up). */
static void paint_key(int r, int c, bool f)
{
    lv_obj_t * k = key_obj(r, c);
    if(!k) return;
    lv_obj_set_style_bg_opa(k, f ? LV_OPA_COVER : LV_OPA_20, 0);
    lv_obj_set_style_text_color(k, lv_color_hex(f ? UI_COLOR_FOCUS_FG : UI_COLOR_FG), 0);
}

static void render_field(void)
{
    char shown[KEYBOARD_MAX + 4];
    snprintf(shown, sizeof(shown), "%s|", text);
    /* a long text shows its end, where the caret is */
    const char * from = shown;
    const lv_font_t * ff = ui_font(34);
    int w = lv_obj_get_width(field);
    while(*from && text_w(ff, from) > w) { from++; while(*from && ((unsigned char)*from & 0xC0) == 0x80) from++; }
    lv_label_set_text(field, from);
}

/* The whole keyboard: on opening, a page change, Shift. */
static void render(void)
{
    const page_t * P = pages[page_i];
    bool up = shift && P->upper[0];
    for(int r = 0; r < MAX_ROWS; r++) split_row(up && P->upper[r] ? P->upper[r] : P->lower[r], r);
    int w = lv_obj_get_width(box);
    int y = PX(96);
    for(int r = 0; r < MAX_ROWS; r++) {
        int total = ncols[r] * key_w() + (ncols[r] - 1) * key_gap();
        int x0 = (w - total) / 2;
        for(int c = 0; c < MAX_COLS; c++) {
            lv_obj_t * k = keys[r][c];
            if(c >= ncols[r]) { lv_obj_add_flag(k, LV_OBJ_FLAG_HIDDEN); continue; }
            lv_obj_remove_flag(k, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(k, x0 + c * (key_w() + key_gap()), y);
            lv_label_set_text(k, keytext[r][c]);
            paint_key(r, c, row == r && col == c);
        }
        if(ncols[r]) y += key_h() + key_gap();
    }
    /* the control row: Shift, the next alphabet, Space (wide), Delete, Done */
    int cw[NCTRL] = { key_w() * 3 / 2, key_w() * 2, key_w() * 4, key_w() * 2, key_w() * 2 };
    int total = 0;
    for(int i = 0; i < NCTRL; i++) total += cw[i];
    total += (NCTRL - 1) * key_gap();
    int x = (w - total) / 2;
    for(int i = 0; i < NCTRL; i++) {
        lv_obj_set_size(ctrl[i], cw[i], key_h());
        lv_obj_set_pos(ctrl[i], x, y);
        lv_label_set_text(ctrl[i], ctrl_text(i));
        paint_key(CTRL_ROW, i, row == CTRL_ROW && col == i);
        x += cw[i] + key_gap();
    }
    lv_obj_set_height(box, y + key_h() + PX(16));
    lv_obj_center(box);
    render_field();
    lv_label_set_text(title_l, title_text);
}

/* ------------------------------------------------------------ editing */

static void type_key(const char * k)
{
    size_t have = strlen(text), add = strlen(k);
    if(have + add > KEYBOARD_MAX) return;
    memcpy(text + have, k, add + 1);
    render_field();
    if(shift == 1) { shift = 0; render(); }     /* one capital, then lower case again */
}

static void delete_last(void)
{
    size_t n = strlen(text);
    if(!n) return;
    n--;
    while(n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80) n--;
    text[n] = 0;
    render_field();
}

static void finish(bool ok)
{
    active = false;
    if(sheet) lv_obj_add_flag(sheet, LV_OBJ_FLAG_HIDDEN);
    size_t n = strlen(text);
    while(n > 0 && text[n - 1] == ' ') text[--n] = 0;
    const char * s = text;
    while(*s == ' ') s++;
    if(ok && *s && done_cb) done_cb(s, done_ctx);
}

static int cols_of(int r) { return r == CTRL_ROW ? NCTRL : ncols[r]; }

static void do_ctrl(int i)
{
    switch(i) {
        case 0: shift = shift == 1 ? 2 : shift == 2 ? 0 : 1; render(); break;
        case 1:
            page_i = (page_i + 1) % npages;
            if(row < CTRL_ROW) { row = 0; col = 0; }
            render();
            if(row < CTRL_ROW && cols_of(row) == 0) { row = CTRL_ROW; col = 1; render(); }
            break;
        case 2: type_key(" "); break;
        case 3: delete_last(); break;
        default: finish(true); return;
    }
}

/* Move the focus: only the old and the new key are repainted. */
static void move_to(int r, int c)
{
    if(r == row && c == col) return;
    paint_key(row, col, false);
    row = r; col = c;
    paint_key(row, col, true);
}

void keyboard_handle_action(ui_action_t a)
{
    if(!active) return;
    switch(a) {
        case ACT_LEFT:  move_to(row, (col + cols_of(row) - 1) % cols_of(row)); break;
        case ACT_RIGHT: move_to(row, (col + 1) % cols_of(row)); break;
        case ACT_UP:
        case ACT_DOWN: {
            int dir = a == ACT_DOWN ? 1 : -1;
            /* the key under (over) this one by position, rows wrapping */
            int from_cols = cols_of(row);
            int r = row;
            for(int tries = 0; tries < MAX_ROWS + 1; tries++) {
                r = (r + dir + MAX_ROWS + 1) % (MAX_ROWS + 1);
                if(cols_of(r) > 0) break;
            }
            int c = (int)((long)(col * 2 + 1) * cols_of(r) / (2 * from_cols));
            if(c >= cols_of(r)) c = cols_of(r) - 1;
            move_to(r, c);
            break;
        }
        case ACT_SELECT:
        case ACT_MENU:
            if(row == CTRL_ROW) do_ctrl(col);
            else if(col < ncols[row]) type_key(keytext[row][col]);
            break;
        case ACT_BACK:  finish(false); break;
        case ACT_NEXT:  delete_last(); break;                   /* X */
        case ACT_PLAYPAUSE: type_key(" "); break;               /* Y */
        case ACT_HOME:  finish(true); break;                    /* Start */
        case ACT_SETTINGS: do_ctrl(1); break;                   /* Select: the next alphabet */
        default: break;
    }
}

bool keyboard_active(void) { return active; }

void keyboard_open(const char * title, const char * initial, void (*done)(const char * text, void * ctx), void * ctx)
{
    if(!sheet) return;
    snprintf(title_text, sizeof(title_text), "%s", title ? title : "");
    snprintf(text, sizeof(text), "%s", initial ? initial : "");
    done_cb = done;
    done_ctx = ctx;
    choose_pages();
    page_i = 0;
    shift = 0;
    row = 1; col = 0;
    active = true;
    lv_obj_remove_flag(sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(sheet);
    render();
}

/* ------------------------------------------------------------ setup */

static lv_obj_t * make_key(lv_obj_t * parent, int w)
{
    lv_obj_t * k = lv_label_create(parent);
    lv_label_set_text(k, "");
    lv_obj_set_style_text_font(k, UI_FONT_LIST, 0);
    lv_obj_set_style_text_align(k, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(k, lv_color_hex(UI_COLOR_FOCUS_BG), 0);
    lv_obj_set_style_bg_opa(k, LV_OPA_20, 0);
    lv_obj_set_style_radius(k, PX(6), 0);
    lv_obj_set_style_pad_ver(k, (key_h() - lv_font_get_line_height(UI_FONT_LIST)) / 2, 0);
    lv_obj_set_size(k, w, key_h());
    return k;
}

void keyboard_create(void)
{
    lv_obj_t * top = lv_layer_top();
    sheet = lv_obj_create(top);
    lv_obj_set_size(sheet, ui_screen.w, ui_screen.h);
    lv_obj_set_pos(sheet, 0, 0);
    lv_obj_set_style_border_width(sheet, 0, 0);
    lv_obj_set_style_radius(sheet, 0, 0);
    lv_obj_set_style_bg_color(sheet, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_70, 0);
    lv_obj_remove_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    box = lv_obj_create(sheet);
    int w = MAX_COLS * key_w() + (MAX_COLS - 1) * key_gap() + PX(32);
    if(w > ui_screen.w - PX(8)) w = ui_screen.w - PX(8);
    lv_obj_set_size(box, w, PX(400));
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_radius(box, PX(10), 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    title_l = lv_label_create(box);
    lv_obj_set_style_text_font(title_l, UI_FONT_HINT, 0);
    lv_obj_set_style_text_color(title_l, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_width(title_l, w - PX(32));
    lv_label_set_long_mode(title_l, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(title_l, PX(16), PX(10));
    field = lv_label_create(box);
    lv_obj_set_style_text_font(field, ui_font(34), 0);
    lv_obj_set_style_text_color(field, lv_color_hex(UI_COLOR_FG), 0);
    lv_obj_set_width(field, w - PX(32));
    lv_label_set_long_mode(field, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_pos(field, PX(16), PX(38));
    for(int r = 0; r < MAX_ROWS; r++) for(int c = 0; c < MAX_COLS; c++) keys[r][c] = make_key(box, key_w());
    for(int i = 0; i < NCTRL; i++) ctrl[i] = make_key(box, key_w() * 2);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_HIDDEN);
}
