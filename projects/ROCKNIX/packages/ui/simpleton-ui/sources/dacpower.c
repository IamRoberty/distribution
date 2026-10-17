/*
 * SimpletonOS UI - waking and sleeping the USB DAC (implementation). See
 * dacpower.h.
 */
#define _GNU_SOURCE
#include "dacpower.h"
#include "config.h"
#include "mpdc.h"

#include <alsa/asoundlib.h>
#include <dirent.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SUSPEND_WAIT_MS   1500      /* how long the cycle waits for the link to go down */
#define HOLD_DOWN_MS       750      /* the link held suspended before the resume: the DX5 II
                                       * ignored ~0 ms (0.21c) and woke at 500 ms (Ian, 16 Oct 18:30) */
#define PROBE_EVERY_MS    2000

static bool     playing, known, our_suspend, started;
static uint32_t last_play_ms, last_poll_ms, last_probe_ms;
static int      validity = -2;     /* last value read: -2 none yet, -1 no such control */
static char     probe_card_name[64];

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static const char * sys_root(void)
{
    const char * e = getenv("SIMPLETON_SYS");          /* the rig's fake /sys */
    return e && e[0] ? e : "/sys";
}

/* The first USB sound card: its card number and its USB device folder
 * (the one with power/control). False when there is none. */
static bool find_dac(int * card, char * dev, size_t len)
{
    char base[256];
    snprintf(base, sizeof(base), "%s/class/sound", sys_root());
    DIR * d = opendir(base);
    if(!d) return false;
    int best = INT_MAX;
    struct dirent * e;
    while((e = readdir(d))) {
        int n;
        char tail;
        if(sscanf(e->d_name, "card%d%c", &n, &tail) != 1) continue;
        char link[600], real[PATH_MAX], ctl[PATH_MAX + 32];
        snprintf(link, sizeof(link), "%.300s/%.256s/device", base, e->d_name);
        if(!realpath(link, real)) continue;
        /* the card's device is a USB interface (1-1:1.0); its parent is the USB device */
        char * slash = strrchr(real, '/');
        if(!slash || !strchr(slash, ':')) continue;
        *slash = 0;
        snprintf(ctl, sizeof(ctl), "%s/power/control", real);
        if(access(ctl, W_OK) != 0) continue;
        if(n < best) { best = n; snprintf(dev, len, "%s", real); }
    }
    closedir(d);
    if(best == INT_MAX) return false;
    *card = best;
    return true;
}

static bool write_attr(const char * dev, const char * name, const char * value)
{
    char path[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/power/%s", dev, name);
    FILE * f = fopen(path, "w");
    if(!f) return false;
    bool ok = fputs(value, f) >= 0;
    if(fclose(f) != 0) ok = false;
    return ok;
}

static void read_attr(const char * dev, const char * name, char * out, size_t len)
{
    char path[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/power/%s", dev, name);
    out[0] = 0;
    FILE * f = fopen(path, "r");
    if(!f) return;
    if(fgets(out, (int)len, f)) out[strcspn(out, "\r\n")] = 0;
    fclose(f);
}

static void product_of(const char * dev, char * out, size_t len)
{
    char path[PATH_MAX + 16];
    snprintf(path, sizeof(path), "%s/product", dev);
    out[0] = 0;
    FILE * f = fopen(path, "r");
    if(!f) { snprintf(out, len, "USB DAC"); return; }
    if(fgets(out, (int)len, f)) out[strcspn(out, "\r\n")] = 0;
    fclose(f);
}

/* ------------------------------------------------------------ wake */

static void wake_cycle(const char * why)
{
    int card;
    char dev[PATH_MAX], name[96], st[32];
    if(!find_dac(&card, dev, sizeof(dev))) return;
    product_of(dev, name, sizeof(name));
    uint32_t t0 = now_ms();
    if(our_suspend) {
        /* we let it sleep: resuming the link wakes it */
        write_attr(dev, "control", "on");
        our_suspend = false;
        read_attr(dev, "runtime_status", st, sizeof(st));
        fprintf(stderr, "simpleton-ui: dac: %s woken (%s; link %s, %u ms)\n", name, why, st, (unsigned)(now_ms() - t0));
        return;
    }
    /* asleep or not, a suspend and a resume of its link wakes it (the DX5 II,
     * 16 Oct: from its own button sleep too) */
    write_attr(dev, "autosuspend_delay_ms", "0");
    write_attr(dev, "control", "auto");
    bool down = false;
    for(uint32_t t = now_ms(); now_ms() - t < SUSPEND_WAIT_MS;) {
        read_attr(dev, "runtime_status", st, sizeof(st));
        if(strcmp(st, "suspended") == 0) { down = true; break; }
        usleep(20000);
    }
    /* held down long enough for the DAC to see it */
    if(down) usleep(HOLD_DOWN_MS * 1000);
    write_attr(dev, "control", "on");
    read_attr(dev, "runtime_status", st, sizeof(st));
    if(down) fprintf(stderr, "simpleton-ui: dac: %s woken by a link suspend/resume (%s; link %s, %u ms)\n", name, why, st, (unsigned)(now_ms() - t0));
    else fprintf(stderr, "simpleton-ui: dac: %s: its link didn't suspend (in use, or it doesn't do runtime suspend); left on (%u ms)\n", name, (unsigned)(now_ms() - t0));
}

void dacpower_before_play(void)
{
    if(playing) return;
    if(!config_read_bool("dac_wake", true)) return;
    uint32_t idle = now_ms() - last_play_ms;
    if(our_suspend) { wake_cycle("it was let sleep"); return; }
    if(!started) { started = true; wake_cycle("the first play since the start"); return; }
    if(idle >= DACPOWER_WAKE_AFTER_S * 1000u) {
        char why[64];
        snprintf(why, sizeof(why), "%u s of silence", (unsigned)(idle / 1000));
        wake_cycle(why);
    }
}

/* ------------------------------------------------------------ state */

void dacpower_mpd_changed(void)
{
    mpd_status_t st;
    if(!mpd_status(&st)) return;
    bool now_playing = strcmp(st.state, "play") == 0;
    if(now_playing) {
        last_play_ms = now_ms();
        started = true;
        if(our_suspend) {
            /* playback started some other way (the remote, the phone later):
             * the open device resumed the link; ours again */
            int card;
            char dev[PATH_MAX];
            if(find_dac(&card, dev, sizeof(dev))) write_attr(dev, "control", "on");
            our_suspend = false;
        }
    }
    else if(playing) last_play_ms = now_ms();          /* silence starts now */
    playing = now_playing;
    known = true;
}

/* The DAC's clock-validity control, read through ALSA (the driver asks the
 * DAC each time, or answers from its cache - the log will tell). */
static void probe(void)
{
    int card;
    char dev[PATH_MAX];
    if(!find_dac(&card, dev, sizeof(dev))) return;
    char hw[16];
    snprintf(hw, sizeof(hw), "hw:%d", card);
    snd_ctl_t * ctl;
    if(snd_ctl_open(&ctl, hw, SND_CTL_NONBLOCK) < 0) return;
    snd_ctl_elem_list_t * list;
    snd_ctl_elem_list_alloca(&list);
    int value = -1;
    char name[64] = "";
    if(snd_ctl_elem_list(ctl, list) == 0) {
        unsigned n = snd_ctl_elem_list_get_count(list);
        if(snd_ctl_elem_list_alloc_space(list, n) == 0 && snd_ctl_elem_list(ctl, list) == 0) {
            for(unsigned i = 0; i < n && value < 0; i++) {
                const char * nm = snd_ctl_elem_list_get_name(list, i);
                if(!nm || !strcasestr(nm, "validity")) continue;
                snd_ctl_elem_value_t * v;
                snd_ctl_elem_value_alloca(&v);
                snd_ctl_elem_id_t * id;
                snd_ctl_elem_id_alloca(&id);
                snd_ctl_elem_list_get_id(list, i, id);
                snd_ctl_elem_value_set_id(v, id);
                if(snd_ctl_elem_read(ctl, v) == 0) { value = snd_ctl_elem_value_get_boolean(v, 0) ? 1 : 0; snprintf(name, sizeof(name), "%s", nm); }
            }
            snd_ctl_elem_list_free_space(list);
        }
    }
    snd_ctl_close(ctl);
    if(value != validity) {
        if(value < 0 && validity == -2) fprintf(stderr, "simpleton-ui: dac: card %d has no clock-validity control\n", card);
        else if(value >= 0) fprintf(stderr, "simpleton-ui: dac: \"%s\" = %s\n", name, value ? "valid" : "NOT valid");
        validity = value;
        snprintf(probe_card_name, sizeof(probe_card_name), "%s", name);
    }
}

void dacpower_poll(void)
{
    uint32_t t = now_ms();
    if(t - last_poll_ms < 1000) return;
    last_poll_ms = t;
    if(!known) return;
    if(playing) {
        if(t - last_probe_ms >= PROBE_EVERY_MS) { last_probe_ms = t; probe(); }
        return;
    }
    /* paused or stopped long enough: let the DAC's link suspend (it sleeps) */
    if(our_suspend) return;
    char v[16];
    config_read("dac_sleep", v, sizeof(v), "300");
    long after = atol(v);
    if(after <= 0 || t - last_play_ms < (uint32_t)after * 1000u) return;
    int card;
    char dev[PATH_MAX], name[96];
    if(!find_dac(&card, dev, sizeof(dev))) return;
    product_of(dev, name, sizeof(name));
    write_attr(dev, "autosuspend_delay_ms", "2000");
    if(write_attr(dev, "control", "auto")) {
        our_suspend = true;
        fprintf(stderr, "simpleton-ui: dac: %s: %ld s without playing, its link may suspend now (it sleeps)\n", name, after);
    }
}

void dacpower_init(void)
{
    last_play_ms = now_ms();
    int card;
    char dev[PATH_MAX], name[96], ctl[32], after[16];
    if(find_dac(&card, dev, sizeof(dev))) {
        product_of(dev, name, sizeof(name));
        read_attr(dev, "control", ctl, sizeof(ctl));
        /* a restart may find the link left on "auto" by the last run: it was
         * ours, the next play resumes it */
        if(strcmp(ctl, "auto") == 0) our_suspend = true;
        config_read("dac_sleep", after, sizeof(after), "300");
        fprintf(stderr, "simpleton-ui: dac: card %d, %s, link power %s; wake on play %s, sleep after %s s\n", card, name, ctl,
                config_read_bool("dac_wake", true) ? "on" : "off", after);
    }
    else fprintf(stderr, "simpleton-ui: dac: no USB DAC yet\n");
    mpd_set_before_play(dacpower_before_play);
}
