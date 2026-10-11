/*
 * SimpletonOS UI - digital silence on the HDMI audio link (implementation).
 * See hdmisilence.h.
 */
#define _GNU_SOURCE
#include "hdmisilence.h"
#include "config.h"

#include <alsa/asoundlib.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define RATE        48000
#define CHANNELS    2
#define BUFFER_US   500000      /* half a second queued: an xrun needs a long stall */
#define PERIOD_US   100000      /* ten wake-ups a second */
#define RETRY_S     5

static char device[64];

/* The HDMI sound card by name, from /proc/asound/cards: the first card whose
 * id has "HDMI" in it and that isn't a USB DAC ("0 [HDMI ]: simple-card -
 * HDMI" on the RGB20SX). Card numbers move from boot to boot, names don't. */
static bool find_card(char * out, size_t len)
{
    const char * dir = getenv("SIMPLETON_ASOUND");
    char path[256];
    snprintf(path, sizeof(path), "%s/cards", dir && dir[0] ? dir : "/proc/asound");
    FILE * f = fopen(path, "r");
    if(!f) return false;
    char line[256];
    bool found = false;
    while(!found && fgets(line, sizeof(line), f)) {
        char * lb = strchr(line, '['), * rb = lb ? strchr(lb, ']') : NULL;
        if(!lb || !rb) continue;
        char id[32];
        size_t n = (size_t)(rb - lb - 1);
        while(n > 0 && lb[n] == ' ') n--;
        if(n == 0 || n >= sizeof(id)) continue;
        memcpy(id, lb + 1, n);
        id[n] = 0;
        if(strstr(rb, "USB-Audio")) continue;
        bool hdmi = false;
        for(size_t i = 0; i + 4 <= n && !hdmi; i++) hdmi = strncasecmp(id + i, "HDMI", 4) == 0;
        if(!hdmi) continue;
        snprintf(out, len, "hw:CARD=%s,DEV=0", id);
        found = true;
    }
    fclose(f);
    return found;
}

static snd_pcm_t * open_pcm(snd_pcm_uframes_t * period, bool quiet)
{
    snd_pcm_t * pcm = NULL;
    int err = snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, 0);
    if(err < 0) return NULL;
    snd_pcm_hw_params_t * hw;
    snd_pcm_hw_params_alloca(&hw);
    unsigned rate = RATE, buffer_us = BUFFER_US, period_us = PERIOD_US;
    int dir = 0;
    if((err = snd_pcm_hw_params_any(pcm, hw)) < 0
       || (err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0
       || (err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0
       || (err = snd_pcm_hw_params_set_channels(pcm, hw, CHANNELS)) < 0
       || (err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, &dir)) < 0
       || (err = snd_pcm_hw_params_set_buffer_time_near(pcm, hw, &buffer_us, &dir)) < 0
       || (err = snd_pcm_hw_params_set_period_time_near(pcm, hw, &period_us, &dir)) < 0
       || (err = snd_pcm_hw_params(pcm, hw)) < 0
       || (err = snd_pcm_hw_params_get_period_size(hw, period, &dir)) < 0
       || (err = snd_pcm_prepare(pcm)) < 0) {
        if(!quiet) fprintf(stderr, "simpleton-ui: hdmi silence: %s: %s\n", device, snd_strerror(err));
        snd_pcm_close(pcm);
        return NULL;
    }
    fprintf(stderr, "simpleton-ui: hdmi silence: %s open, %u Hz, %lu-frame periods\n", device, rate, (unsigned long)*period);
    return pcm;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* alsa-lib prints its own line on every failed open; ours says it once */
static void alsa_quiet(const char * file, int line, const char * func, int err, const char * fmt, ...)
{
    (void)file; (void)line; (void)func; (void)err; (void)fmt;
}

static void * silence_thread(void * arg)
{
    (void)arg;
    snd_lib_error_set_handler(alsa_quiet);
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);   /* the UI comes first */
    bool told = false;
    for(;;) {
        snd_pcm_uframes_t period = 0;
        snd_pcm_t * pcm = open_pcm(&period, told);
        if(!pcm || period == 0 || period > RATE) {
            if(pcm) snd_pcm_close(pcm);
            if(!told) fprintf(stderr, "simpleton-ui: hdmi silence: can't open %s, trying every %d s\n", device, RETRY_S);
            told = true;
            sleep(RETRY_S);
            continue;
        }
        told = false;
        int16_t * zeros = calloc((size_t)period * CHANNELS, sizeof(int16_t));
        if(!zeros) { snd_pcm_close(pcm); sleep(RETRY_S); continue; }
        int quick = 0;
        for(;;) {
            double t0 = now_s();
            snd_pcm_sframes_t n = snd_pcm_writei(pcm, zeros, period);
            if(n < 0) n = snd_pcm_recover(pcm, (int)n, 1);     /* underrun, resume after suspend */
            if(n < 0) {
                fprintf(stderr, "simpleton-ui: hdmi silence: %s: %s, reopening\n", device, snd_strerror((int)n));
                break;
            }
            /* a device that never blocks would spin: after a run of instant
             * writes, wait a period's time ourselves */
            if(now_s() - t0 < 0.001) { if(++quick >= 10) usleep(PERIOD_US); }
            else quick = 0;
        }
        free(zeros);
        snd_pcm_close(pcm);
        sleep(1);
    }
    return NULL;
}

bool hdmi_silence_start(void)
{
    if(!config_read_bool("hdmi_silence", true)) {
        fprintf(stderr, "simpleton-ui: hdmi silence: off (hdmi_silence setting)\n");
        return false;
    }
    const char * test = getenv("SIMPLETON_HDMI_PCM");
    if(test && test[0]) snprintf(device, sizeof(device), "%s", test);
    else if(!find_card(device, sizeof(device))) {
        fprintf(stderr, "simpleton-ui: hdmi silence: no HDMI sound card in /proc/asound/cards\n");
        return false;
    }
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 * 1024);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int err = pthread_create(&th, &at, silence_thread, NULL);
    pthread_attr_destroy(&at);
    if(err != 0) { fprintf(stderr, "simpleton-ui: hdmi silence: no thread (%s)\n", strerror(err)); return false; }
    return true;
}
