/*
 * SimpletonOS UI - thumbnail decoder (implementation). See thumbs.h.
 *
 * One worker thread, two lists under one mutex: `want` (replaced whole by
 * the grid) and `done` (a short queue the UI thread drains). The worker
 * takes the first wanted entry that is not already done or in flight,
 * decodes it with art_decode_fit() (thread-safe since 0.16), and appends
 * the result. Nothing here touches LVGL.
 */
#include "thumbs.h"
#include "art.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DONE_MAX 64

static pthread_t       worker;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  wake = PTHREAD_COND_INITIALIZER;
static bool            running;

static thumb_req_t want[THUMBS_MAX_WANT];
static int          want_count;
static thumb_res_t  done[DONE_MAX];
static int          done_count;
static int          flight_id = -1, flight_px, flight_gen;

static uint8_t * read_file(const char * path, size_t * len)
{
    FILE * f = fopen(path, "rb");
    if(!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if(n <= 0 || n > 8 * 1024 * 1024) { fclose(f); return NULL; }
    uint8_t * buf = malloc((size_t)n);
    if(!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if(got != (size_t)n) { free(buf); return NULL; }
    *len = got;
    return buf;
}

static bool is_done(int id, int gen, int px)
{
    for(int i = 0; i < done_count; i++) if(done[i].id == id && done[i].gen == gen && done[i].px == px) return true;
    return false;
}

static void * worker_main(void * arg)
{
    (void)arg;
    pthread_mutex_lock(&lock);
    while(running) {
        /* the first wanted entry not yet delivered; wait while the done
         * queue is full so results are never lost */
        int pick = -1;
        if(done_count < DONE_MAX)
            for(int i = 0; i < want_count; i++)
                if(!is_done(want[i].id, want[i].gen, want[i].px)) { pick = i; break; }
        if(pick < 0) { pthread_cond_wait(&wake, &lock); continue; }

        thumb_req_t req = want[pick];
        flight_id = req.id;
        flight_px = req.px;
        flight_gen = req.gen;
        pthread_mutex_unlock(&lock);

        thumb_res_t res = { .id = req.id, .gen = req.gen, .px = req.px, .pixels = NULL, .w = 0, .h = 0 };
        size_t len = 0;
        uint8_t * data = read_file(req.path, &len);
        if(data) {
            res.pixels = art_decode_fit(data, len, req.px, &res.w, &res.h);
            free(data);
        }

        pthread_mutex_lock(&lock);
        flight_id = -1;
        /* the result is kept even if the want list moved on: the grid's
         * own cache keeps or drops it */
        if(done_count < DONE_MAX) done[done_count++] = res;
        else free(res.pixels);
        /* drop it from want so it isn't picked again */
        for(int i = 0; i < want_count; i++)
            if(want[i].id == req.id && want[i].gen == req.gen && want[i].px == req.px) { memmove(&want[i], &want[i + 1], sizeof(want[0]) * (size_t)(want_count - i - 1)); want_count--; break; }
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

bool thumbs_init(void)
{
    if(running) return true;
    running = true;
    if(pthread_create(&worker, NULL, worker_main, NULL) != 0) { running = false; return false; }
    pthread_detach(worker);
    return true;
}

void thumbs_want(const thumb_req_t * reqs, int count)
{
    if(count > THUMBS_MAX_WANT) count = THUMBS_MAX_WANT;
    pthread_mutex_lock(&lock);
    want_count = 0;
    for(int i = 0; i < count; i++) {
        /* skip what is already decoded or being decoded */
        if(is_done(reqs[i].id, reqs[i].gen, reqs[i].px)) continue;
        if(reqs[i].id == flight_id && reqs[i].gen == flight_gen && reqs[i].px == flight_px) continue;
        want[want_count++] = reqs[i];
    }
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
}

bool thumbs_poll(thumb_res_t * out)
{
    pthread_mutex_lock(&lock);
    bool any = done_count > 0;
    if(any) {
        *out = done[0];
        memmove(&done[0], &done[1], sizeof(done[0]) * (size_t)(done_count - 1));
        done_count--;
        pthread_cond_signal(&wake);
    }
    pthread_mutex_unlock(&lock);
    return any;
}
