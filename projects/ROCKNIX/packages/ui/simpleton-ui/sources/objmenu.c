/*
 * SimpletonOS UI - the object menu (implementation). See objmenu.h.
 */
#include "objmenu.h"
#include "curation.h"
#include "keyboard.h"
#include "layout.h"
#include "strings.h"
#include "theme.h"
#include "lvgl.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROWS_ON   6                      /* rows of the second list on screen at once */
#define TOAST_MS  3000

typedef enum { LEVEL_NONE, LEVEL_MAIN, LEVEL_PICK } level_t;
typedef enum { ACT_FAV, ACT_UNFAV, ACT_TO_MIXTAPE, ACT_TO_COLLECTION, ACT_REMOVE, ACT_RENAME, ACT_DELETE, ACT_CANCEL } main_act_t;

/* the target */
static bool    is_album, is_list;
static char    t_uri[1024], t_title[300], t_artist[300];
static char ** t_uris;
static int     t_n;
static int     in_uid, in_pos, list_uid;

/* the menu */
static level_t   level;
static main_act_t main_acts[7];
static int       nmain, focus, first;
static cur_kind_t pick_kind;
static int       npick;                  /* "+ New" and the lists */
static int       last_used[2];           /* uid last added to, per kind */

static lv_obj_t * sheet, * box, * header, * title_l, * rows[ROWS_ON], * toast;
static lv_timer_t * toast_timer;
static void (*changed_cb)(void);

/* ------------------------------------------------------------ drawing */

static int row_h(void) { return PX(52); }

static void layout_box(int n)
{
    int w = PX(440), h = PX(60) + row_h() * n + PX(12);
    if(w > ui_screen.w - PX(16)) w = ui_screen.w - PX(16);
    lv_obj_set_size(box, w, h);
    lv_obj_center(box);
    lv_obj_set_width(header, w - PX(32));
    lv_obj_set_width(title_l, w - PX(32));
    for(int i = 0; i < ROWS_ON; i++) {
        lv_obj_set_width(rows[i], w - PX(32));
        lv_obj_set_pos(rows[i], 0, PX(52) + row_h() * i);
        if(i < n) lv_obj_remove_flag(rows[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static const char * list_name(int uid)
{
    const cur_list_t * l = curation_by_uid(uid);
    return l && l->name ? l->name : "";
}

static void main_text(int i, char * out, size_t len)
{
    switch(main_acts[i]) {
        case ACT_FAV:           snprintf(out, len, "%s", T(S_MENU_FAVORITE)); break;
        case ACT_UNFAV:         snprintf(out, len, "%s", T(S_MENU_UNFAVORITE)); break;
        case ACT_TO_MIXTAPE:    snprintf(out, len, "%s", T(S_MENU_ADD_MIXTAPE)); break;
        case ACT_TO_COLLECTION: snprintf(out, len, "%s", T(S_MENU_ADD_COLLECTION)); break;
        case ACT_REMOVE:        ui_strf(out, len, S_MENU_REMOVE_FROM, "name", list_name(in_uid), NULL); break;
        case ACT_RENAME:        snprintf(out, len, "%s", T(S_MENU_RENAME)); break;
        case ACT_DELETE:        ui_strf(out, len, S_MENU_DELETE, "name", list_name(list_uid), NULL); break;
        default:                snprintf(out, len, "%s", T(S_MENU_CANCEL)); break;
    }
}

static int pick_count(void) { return 1 + curation_count(pick_kind); }

static const char * pick_text(int i)
{
    if(i == 0) return T(pick_kind == CUR_MIXTAPE ? S_MENU_NEW_MIXTAPE : S_MENU_NEW_COLLECTION);
    const cur_list_t * l = curation_get(pick_kind, i - 1);
    return l && l->name ? l->name : "";
}

static void render(void)
{
    int n = level == LEVEL_MAIN ? nmain : npick;
    int on = n < ROWS_ON ? n : ROWS_ON;
    if(focus < first) first = focus;
    if(focus >= first + on) first = focus - on + 1;
    layout_box(on);
    for(int i = 0; i < on; i++) {
        int k = first + i;
        char text[400];
        if(level == LEVEL_MAIN) main_text(k, text, sizeof(text)); else snprintf(text, sizeof(text), "%s", pick_text(k));
        lv_label_set_text(rows[i], text);
        bool f = k == focus;
        lv_obj_set_style_bg_opa(rows[i], f ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(rows[i], lv_color_hex(f ? UI_COLOR_FOCUS_FG : UI_COLOR_FG), 0);
    }
    lv_label_set_text(header, level == LEVEL_PICK ? T(pick_kind == CUR_MIXTAPE ? S_MENU_PICK_MIXTAPE : S_MENU_PICK_COLLECTION) : t_artist);
    lv_label_set_text(title_l, t_title);
}

static void toast_hide_cb(lv_timer_t * t)
{
    (void)t;
    if(toast) lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
    toast_timer = NULL;
}

void objmenu_toast(const char * text)
{
    if(!toast) return;
    lv_label_set_text(toast, text);
    lv_obj_remove_flag(toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(toast);
    if(toast_timer) lv_timer_delete(toast_timer);
    toast_timer = lv_timer_create(toast_hide_cb, TOAST_MS, NULL);
    lv_timer_set_repeat_count(toast_timer, 1);
}

static void toast_f(int id, const char * name, int n)
{
    char text[400], num[16];
    snprintf(num, sizeof(num), "%d", n);
    ui_strf(text, sizeof(text), (str_id_t)id, "name", name ? name : "", "n", num, NULL);
    objmenu_toast(text);
}

/* ------------------------------------------------------------ opening */

static void target_clear(void)
{
    for(int i = 0; i < t_n; i++) free(t_uris[i]);
    free(t_uris);
    t_uris = NULL;
    t_n = 0;
}

static void open_box(void)
{
    level = LEVEL_MAIN;
    focus = first = 0;
    lv_obj_remove_flag(sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(sheet);
    if(toast) lv_obj_move_foreground(toast);
    render();
}

static void build_main(void)
{
    nmain = 0;
    if(is_list) { main_acts[nmain++] = ACT_RENAME; main_acts[nmain++] = ACT_DELETE; }
    else {
        if(!is_album) main_acts[nmain++] = curation_is_favorite(t_uri) ? ACT_UNFAV : ACT_FAV;
        if(is_album) main_acts[nmain++] = ACT_TO_COLLECTION;
        main_acts[nmain++] = ACT_TO_MIXTAPE;
        if(in_uid > 0 && in_pos >= 0) main_acts[nmain++] = ACT_REMOVE;
    }
    main_acts[nmain++] = ACT_CANCEL;
}

void objmenu_open_track(const char * uri, const char * title, const char * artist, int uid, int pos)
{
    if(!sheet || !uri) return;
    target_clear();
    is_album = is_list = false;
    snprintf(t_uri, sizeof(t_uri), "%s", uri);
    snprintf(t_title, sizeof(t_title), "%s", title ? title : "");
    snprintf(t_artist, sizeof(t_artist), "%s", artist ? artist : "");
    t_uris = calloc(1, sizeof(char *));
    if(t_uris) { t_uris[0] = strdup(uri); t_n = 1; }
    in_uid = uid; in_pos = pos;
    build_main();
    open_box();
}

void objmenu_open_album(const char * folder, const char * title, const char * artist, const char * const * uris, int n, int uid, int pos)
{
    if(!sheet || !folder) return;
    target_clear();
    is_album = true; is_list = false;
    snprintf(t_uri, sizeof(t_uri), "%s", folder);
    snprintf(t_title, sizeof(t_title), "%s", title ? title : "");
    snprintf(t_artist, sizeof(t_artist), "%s", artist ? artist : "");
    t_uris = n > 0 ? calloc((size_t)n, sizeof(char *)) : NULL;
    if(t_uris) { for(int i = 0; i < n; i++) t_uris[i] = strdup(uris[i] ? uris[i] : ""); t_n = n; }
    in_uid = uid; in_pos = pos;
    build_main();
    open_box();
}

void objmenu_open_list(int uid)
{
    const cur_list_t * l = curation_by_uid(uid);
    if(!sheet || !l) return;
    target_clear();
    is_album = false; is_list = true;
    list_uid = uid;
    snprintf(t_title, sizeof(t_title), "%s", l->name ? l->name : "");
    snprintf(t_artist, sizeof(t_artist), "%s", T(l->kind == CUR_MIXTAPE ? S_HOME_MIXTAPES : S_HOME_COLLECTIONS));
    in_uid = 0; in_pos = -1;
    build_main();
    open_box();
}

bool objmenu_active(void) { return level != LEVEL_NONE; }

static void close_box(void)
{
    level = LEVEL_NONE;
    if(sheet) lv_obj_add_flag(sheet, LV_OBJ_FLAG_HIDDEN);
}

static void changed(void) { if(changed_cb) changed_cb(); }

/* ------------------------------------------------------------ doing */

static const char * target_card(char * out, size_t len)
{
    return curation_card_of(t_n > 0 ? t_uris[0] : t_uri, out, len);
}

/* The first unused "Mixtape n" / "Collection n". */
static void new_name(cur_kind_t k, char * out, size_t len)
{
    for(int n = 1; n < 100000; n++) {
        char num[16];
        snprintf(num, sizeof(num), "%d", n);
        ui_strf(out, len, k == CUR_MIXTAPE ? S_NAME_MIXTAPE : S_NAME_COLLECTION, "n", num, NULL);
        if(!curation_name_used(k, out)) return;
    }
}

static void add_to(int uid)
{
    const cur_list_t * l = curation_by_uid(uid);
    if(!l) return;
    bool ok;
    if(l->kind == CUR_MIXTAPE) {
        int added = 0;
        ok = curation_add_tracks(uid, (const char * const *)t_uris, t_n, &added);
        if(added == 0) toast_f(S_TOAST_ALREADY, l->name, 0);
        else if(!ok) objmenu_toast(T(S_TOAST_NOT_SAVED));
        else if(is_album) toast_f(S_TOAST_ADDED_N, l->name, added);
        else toast_f(S_TOAST_ADDED, l->name, 1);
    }
    else {
        bool added = false;
        ok = curation_add_album(uid, t_uri, t_title, t_artist, &added);
        if(!added && ok) toast_f(S_TOAST_ALREADY, l->name, 0);
        else if(!ok) objmenu_toast(T(S_TOAST_NOT_SAVED));
        else toast_f(S_TOAST_ADDED, l->name, 1);
    }
    last_used[l->kind] = uid;
    changed();
}

/* The keyboard's answer (0.21a): the list takes the name. */
static void renamed(const char * name, void * ctx)
{
    int uid = (int)(intptr_t)ctx;
    const cur_list_t * l = curation_by_uid(uid);
    if(!l) return;
    if(curation_rename(uid, name)) toast_f(S_TOAST_RENAMED, name, 0); else objmenu_toast(T(S_TOAST_NOT_SAVED));
    changed();
}

static void name_list(int uid)
{
    const cur_list_t * l = curation_by_uid(uid);
    if(!l) return;
    keyboard_open(T(l->kind == CUR_MIXTAPE ? S_KB_NAME_MIXTAPE : S_KB_NAME_COLLECTION), l->name, renamed, (void *)(intptr_t)uid);
}

static void do_main(void)
{
    switch(main_acts[focus]) {
        case ACT_FAV:
        case ACT_UNFAV: {
            bool on = main_acts[focus] == ACT_FAV;
            char card[64];
            target_card(card, sizeof(card));
            if(!card[0]) { objmenu_toast(T(S_TOAST_NO_CARD)); break; }
            bool ok = curation_set_favorite(t_uri, on);
            if(!ok && !curation_is_favorite(t_uri) && on) objmenu_toast(T(S_TOAST_NO_CARD));
            else objmenu_toast(T(ok ? (on ? S_TOAST_FAVORITE : S_TOAST_UNFAVORITE) : S_TOAST_NOT_SAVED));
            changed();
            break;
        }
        case ACT_TO_MIXTAPE:
        case ACT_TO_COLLECTION: {
            pick_kind = main_acts[focus] == ACT_TO_MIXTAPE ? CUR_MIXTAPE : CUR_COLLECTION;
            level = LEVEL_PICK;
            npick = pick_count();
            focus = first = 0;
            for(int i = 1; i < npick; i++) if(curation_get(pick_kind, i - 1)->uid == last_used[pick_kind]) focus = i;
            render();
            return;
        }
        case ACT_REMOVE: {
            const cur_list_t * l = curation_by_uid(in_uid);
            bool ok = curation_remove_at(in_uid, in_pos);
            if(ok) toast_f(S_TOAST_REMOVED, l ? l->name : "", 0); else objmenu_toast(T(S_TOAST_NOT_SAVED));
            changed();
            break;
        }
        case ACT_RENAME:
            close_box();
            name_list(list_uid);
            return;
        case ACT_DELETE: {
            char name[300];
            snprintf(name, sizeof(name), "%s", list_name(list_uid));
            if(curation_delete(list_uid)) toast_f(S_TOAST_DELETED, name, 0); else objmenu_toast(T(S_TOAST_NOT_SAVED));
            changed();
            break;
        }
        default: break;
    }
    close_box();
}

static void do_pick(void)
{
    int uid;
    if(focus == 0) {
        char card[64], name[200];
        target_card(card, sizeof(card));
        new_name(pick_kind, name, sizeof(name));
        uid = curation_new(pick_kind, card, name);
        if(!uid) { objmenu_toast(T(card[0] ? S_TOAST_NOT_SAVED : S_TOAST_NO_CARD)); close_box(); return; }
        add_to(uid);
        close_box();
        name_list(uid);               /* name it now (Ian, 15 Oct); Back keeps "Mixtape 1" */
        return;
    }
    else {
        const cur_list_t * l = curation_get(pick_kind, focus - 1);
        if(!l) { close_box(); return; }
        uid = l->uid;
    }
    add_to(uid);
    close_box();
}

void objmenu_handle_action(ui_action_t a)
{
    if(level == LEVEL_NONE) return;
    int n = level == LEVEL_MAIN ? nmain : npick;
    switch(a) {
        case ACT_UP:     focus = (focus + n - 1) % n; render(); break;
        case ACT_DOWN:   focus = (focus + 1) % n; render(); break;
        case ACT_SELECT: if(level == LEVEL_MAIN) do_main(); else do_pick(); break;
        case ACT_BACK:
        case ACT_MENU:
            if(level == LEVEL_PICK) { level = LEVEL_MAIN; focus = first = 0; build_main(); render(); }
            else close_box();
            break;
        default: break;
    }
}

/* ------------------------------------------------------------ the heart */

#define HEART_SIZES 8
static struct { int r; uint32_t colour; lv_image_dsc_t dsc; uint8_t * px; } hearts[HEART_SIZES];

const lv_image_dsc_t * curation_heart_dsc(int r, uint32_t colour)
{
    if(r < 3) r = 3;
    for(int i = 0; i < HEART_SIZES; i++) if(hearts[i].r == r && hearts[i].colour == colour) return &hearts[i].dsc;
    int slot = -1;
    for(int i = 0; i < HEART_SIZES && slot < 0; i++) if(hearts[i].r == 0) slot = i;
    if(slot < 0) slot = HEART_SIZES - 1;
    int sz = 2 * r + 2;
    uint32_t stride = (uint32_t)sz * 4;
    uint8_t * px = calloc((size_t)stride * (size_t)sz, 1);
    if(!px) return NULL;
    free(hearts[slot].px);
    hearts[slot].px = px;
    hearts[slot].r = r;
    hearts[slot].colour = colour;
    lv_image_dsc_t * d = &hearts[slot].dsc;
    memset(d, 0, sizeof(*d));
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_ARGB8888;
    d->header.w = (uint32_t)sz;
    d->header.h = (uint32_t)sz;
    d->header.stride = stride;
    d->data_size = stride * (uint32_t)sz;
    d->data = px;
    /* drawn into a canvas: two lobes and a point, as the album page draws it */
    lv_obj_t * c = lv_canvas_create(lv_layer_top());
    lv_canvas_set_draw_buf(c, lv_draw_buf_create((uint32_t)sz, (uint32_t)sz, LV_COLOR_FORMAT_ARGB8888, stride));
    lv_canvas_fill_bg(c, lv_color_black(), LV_OPA_TRANSP);
    lv_layer_t layer;
    lv_canvas_init_layer(c, &layer);
    lv_draw_rect_dsc_t rc;
    lv_draw_rect_dsc_init(&rc);
    rc.bg_color = lv_color_hex(colour);
    rc.bg_opa = LV_OPA_COVER;
    rc.radius = LV_RADIUS_CIRCLE;
    int cx = r + 1, cy = r + 1, rad = r / 2;
    lv_area_t a1 = { cx - r, cy - r * 9 / 10, cx, cy - r * 9 / 10 + 2 * rad };
    lv_area_t a2 = { cx, cy - r * 9 / 10, cx + r, cy - r * 9 / 10 + 2 * rad };
    lv_draw_rect(&layer, &rc, &a1);
    lv_draw_rect(&layer, &rc, &a2);
    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    t.color = lv_color_hex(colour);
    t.opa = LV_OPA_COVER;
    t.p[0].x = cx - r * 98 / 100; t.p[0].y = cy - r * 2 / 10;
    t.p[1].x = cx + r * 98 / 100; t.p[1].y = cy - r * 2 / 10;
    t.p[2].x = cx;                t.p[2].y = cy + r;
    lv_draw_triangle(&layer, &t);
    lv_canvas_finish_layer(c, &layer);
    lv_draw_buf_t * buf = lv_canvas_get_draw_buf(c);
    for(int y = 0; y < sz; y++) memcpy(px + (size_t)y * stride, (const uint8_t *)buf->data + (size_t)y * buf->header.stride, stride);
    lv_draw_buf_destroy(buf);
    lv_obj_delete(c);
    return d;
}

lv_obj_t * curation_heart(lv_obj_t * parent, int r, uint32_t colour)
{
    const lv_image_dsc_t * d = curation_heart_dsc(r, colour);
    if(!d) return NULL;
    lv_obj_t * img = lv_image_create(parent);
    lv_image_set_src(img, d);
    lv_obj_set_size(img, (int32_t)d->header.w, (int32_t)d->header.h);
    return img;
}

/* ------------------------------------------------------------ setup */

void objmenu_create(void (*on_changed)(void))
{
    changed_cb = on_changed;
    lv_obj_t * top = lv_layer_top();
    sheet = lv_obj_create(top);
    lv_obj_set_size(sheet, ui_screen.w, ui_screen.h);
    lv_obj_set_pos(sheet, 0, 0);
    lv_obj_set_style_border_width(sheet, 0, 0);
    lv_obj_set_style_radius(sheet, 0, 0);
    lv_obj_set_style_bg_color(sheet, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_60, 0);
    lv_obj_remove_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    box = lv_obj_create(sheet);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_radius(box, PX(10), 0);
    lv_obj_set_style_pad_all(box, PX(8), 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    header = lv_label_create(box);
    lv_obj_set_style_text_font(header, UI_FONT_HINT, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(UI_COLOR_DIM), 0);
    lv_label_set_long_mode(header, LV_LABEL_LONG_DOT);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, PX(12), PX(4));
    title_l = lv_label_create(box);
    lv_obj_set_style_text_font(title_l, UI_FONT_HINT, 0);
    lv_obj_set_style_text_color(title_l, lv_color_hex(UI_COLOR_FG), 0);
    lv_label_set_long_mode(title_l, LV_LABEL_LONG_DOT);
    lv_obj_align(title_l, LV_ALIGN_TOP_LEFT, PX(12), PX(26));
    for(int i = 0; i < ROWS_ON; i++) {
        lv_obj_t * r = lv_label_create(box);
        lv_label_set_text(r, "");
        lv_obj_set_style_text_font(r, UI_FONT_LIST, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(UI_COLOR_FOCUS_BG), 0);
        lv_obj_set_style_radius(r, PX(6), 0);
        lv_obj_set_style_pad_hor(r, PX(10), 0);
        lv_obj_set_style_pad_ver(r, PX(8), 0);
        lv_label_set_long_mode(r, LV_LABEL_LONG_DOT);
        rows[i] = r;
    }
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_HIDDEN);

    toast = lv_label_create(top);
    lv_obj_set_style_text_font(toast, UI_FONT_LIST, 0);
    lv_obj_set_style_text_color(toast, lv_color_hex(UI_COLOR_FOCUS_FG), 0);
    lv_obj_set_style_bg_color(toast, lv_color_hex(UI_COLOR_FOCUS_BG), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_90, 0);
    lv_obj_set_style_radius(toast, PX(10), 0);
    lv_obj_set_style_pad_all(toast, PX(12), 0);
    lv_obj_set_width(toast, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(toast, ui_screen.w - PX(32), 0);
    lv_label_set_long_mode(toast, LV_LABEL_LONG_DOT);
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -PX(90));
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
}
