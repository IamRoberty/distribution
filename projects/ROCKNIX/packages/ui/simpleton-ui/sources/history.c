/*
 * SimpletonOS UI - what was played (implementation). See history.h.
 */
#include "history.h"
#include "config.h"
#include "mpdc.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NOTE_AFTER_MS 10000

static char *   uris[HISTORY_MAX];
static int      count;
static unsigned generation;
static char     pending[1024];      /* the song that just became current */
static char     last_seen[1024];    /* the current song at the last look */
static lv_timer_t * note_timer;
static void (*changed_cb)(void);
static char     path[600];

const char * history_path(void)
{
    if(!path[0]) {
        const char * env = getenv("SIMPLETON_HISTORY");
        if(env && env[0]) snprintf(path, sizeof(path), "%s", env);
        else config_path("history", path, sizeof(path));
    }
    return path;
}

int history_count(void) { return count; }
const char * history_get(int i) { return (i >= 0 && i < count) ? uris[i] : NULL; }
unsigned history_generation(void) { return generation; }

static void load(void)
{
    FILE * f = fopen(history_path(), "r");
    if(!f) return;
    char line[1100];
    while(count < HISTORY_MAX && fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while(n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if(!n) continue;
        uris[count++] = strdup(line);
    }
    fclose(f);
}

static void save(void)
{
    char tmp[620];
    snprintf(tmp, sizeof(tmp), "%s.tmp", history_path());
    FILE * f = fopen(tmp, "w");
    if(!f) return;
    for(int i = 0; i < count; i++) fprintf(f, "%s\n", uris[i]);
    if(fclose(f) == 0) rename(tmp, history_path());
}

static void note(const char * uri)
{
    if(!uri || !uri[0]) return;
    int at = -1;
    for(int i = 0; i < count; i++) if(strcmp(uris[i], uri) == 0) { at = i; break; }
    if(at == 0) return;                                     /* already on top */
    char * u = at >= 0 ? uris[at] : strdup(uri);
    if(!u) return;
    if(at < 0) {
        if(count == HISTORY_MAX) { free(uris[count - 1]); count--; }
        at = count++;
    }
    memmove(&uris[1], &uris[0], sizeof(uris[0]) * (size_t)at);
    uris[0] = u;
    generation++;
    save();
    fprintf(stderr, "simpleton-ui: history: %s (%d kept)\n", uri, count);
    if(changed_cb) changed_cb();
}

static void note_cb(lv_timer_t * t)
{
    (void)t;
    note_timer = NULL;
    mpd_status_t st;
    if(!pending[0] || !mpd_status(&st) || !st.file[0]) return;
    if(strcmp(st.file, pending) == 0 && strcmp(st.state, "stop") != 0) note(pending);
    pending[0] = 0;
}

void history_mpd_changed(void)
{
    mpd_status_t st;
    if(!mpd_status(&st)) return;
    if(!st.file[0] || strcmp(st.state, "stop") == 0) { last_seen[0] = 0; return; }
    if(strcmp(st.file, last_seen) == 0) return;             /* a pause, a seek */
    snprintf(last_seen, sizeof(last_seen), "%s", st.file);
    snprintf(pending, sizeof(pending), "%s", st.file);
    if(note_timer) lv_timer_reset(note_timer);
    else { note_timer = lv_timer_create(note_cb, NOTE_AFTER_MS, NULL); lv_timer_set_repeat_count(note_timer, 1); }
}

void history_init(void (*changed)(void))
{
    changed_cb = changed;
    load();
    fprintf(stderr, "simpleton-ui: history: %d track%s played before\n", count, count == 1 ? "" : "s");
}
