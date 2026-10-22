/*
 * SimpletonOS UI - the visualizers (implementation). See viz.h and
 * visualizers-research.md; the analysis and the GL path come from the 0.30
 * test kit (viztest.c), which proved them on the unit.
 */
#define _GNU_SOURCE
#include "viz.h"
#include "config.h"
#include "display.h"
#include "layout.h"
#include "mpdc.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define FIFO_PATH   "/tmp/simpleton-viz.fifo"
#define FEED_NAME   "viz"
static int   sr = 44100;              /* the analysis rate: the feed brought down to 44.1 or 48 kHz */
static volatile int delay_samples;    /* the sync delay (0.35), in samples at sr */
static int delay_ms_set = 400;
#define FFT_N       4096
#define HOP         1024
#define ROWS        17                   /* at most; the board picks how many fit */
#define ACROSS_LOW  15                   /* bulbs across the bottom row (Ian, 18 Oct 21:49) */
#define ACROSS_HIGH 50                   /* ...and the top row */
static int nrows = ROWS;
#define BULBS_MAX   1400
#define PAN_SLOTS   96                   /* the stereo picture of a row, left to right, per hop */
#define FRAMES_PER_BEAT 8
#define FPS_STEADY  15
#define FPS_MIN     10
#define FPS_MAX     20                   /* 0.31f: 30 starved MPD's DST decoder on the unit; 24 → 20 in 0.31i */
#define NICE_AUDIO  10                   /* 0.31f: the music comes first - both threads give way */
#define NICE_GPU    15                   /*        to MPD and the UI (DSD skips crackled at 0) */

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void sleep_s(double s) { if(s > 0) { struct timespec t = { (time_t)s, (long)((s - (time_t)s) * 1e9) }; nanosleep(&t, NULL); } }

/* ------------------------------------------------------------ analysis */

typedef struct {
    float rms_l, rms_r, peak, corr;         /* the last second */
    float band[ROWS];                        /* energy per row, 0..1 after gain */
    float sband[VIZ_BANDS];                  /* the classics' spectrum: 48 log bands, 0..1 (0.32) */
    float level_l, level_r;                  /* the last hop's RMS, 0..1 (0.32) */
    float spg[VIZ_SPG_ROWS];                 /* the spectrogram's column: 30 Hz - 18 kHz by log, 0..1 (0.33) */
    float hist[ROWS][PAN_SLOTS];             /* each row's energy by stereo position, the last hop */
    float bpm, bpm_conf;
    int   loud_bin;
    long  bytes_s;                           /* bytes read in the last second */
    int   stalls;
    bool  fresh;
} analysis_t;

static analysis_t an;
static pthread_mutex_t an_lock = PTHREAD_MUTEX_INITIALIZER;

static void fft(float * re, float * im, int n)
{
    for(int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for(; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if(i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for(int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * (float)M_PI / (float)len;
        float wr = cosf(ang), wi = sinf(ang);
        for(int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for(int k = 0; k < len / 2; k++) {
                int a = i + k, b = i + k + len / 2;
                float xr = re[b] * cr - im[b] * ci, xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr; im[b] = im[a] - xi; re[a] += xr; im[a] += xi;
                float t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
            }
        }
    }
}

/* the rows' frequency edges: log scale, 25 Hz at the bottom to 18 kHz at the top */
static float row_lo(int r) { return 25.0f * powf(18000.0f / 25.0f, (float)r / nrows); }
static int   row_k0[ROWS], row_k1[ROWS], row_count[ROWS];       /* each row's FFT bins and bulbs (build_board) */

#define ONSET_HIST 400
static float onset_hist[ONSET_HIST]; static int onset_n;
static float prev_logmag[FFT_N / 2];
static float band_max[ROWS];
static float onset_mean; static int hops_since_beat; static volatile unsigned beats;
static float sband_max[VIZ_BANDS];
static int   sb_k0[VIZ_BANDS + 1];           /* the classics' band edges in FFT bins (set_rate) */
static float spg_k[VIZ_SPG_ROWS];            /* the spectrogram rows' centre bins (fractional) */
static float spg_max;                        /* its slow auto gain */

static void analyse_hop(const int16_t * ring, int ring_len, int head)
{
    static float re[FFT_N], im[FFT_N], win[FFT_N]; static bool win_ok;
    if(!win_ok) { for(int i = 0; i < FFT_N; i++) win[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (FFT_N - 1)); win_ok = true; }
    /* L into the real part, R into the imaginary: one FFT for both */
    for(int i = 0; i < FFT_N; i++) {
        int k = (head - FFT_N + i + ring_len) % ring_len;
        re[i] = ring[2 * k] / 32768.0f * win[i];
        im[i] = ring[2 * k + 1] / 32768.0f * win[i];
    }
    fft(re, im, FFT_N);
    float ml[FFT_N / 2], mr[FFT_N / 2];
    for(int k = 1; k < FFT_N / 2; k++) {
        float ar = (re[k] + re[FFT_N - k]) * 0.5f, ai = (im[k] - im[FFT_N - k]) * 0.5f;      /* L */
        float br = (im[k] + im[FFT_N - k]) * 0.5f, bi = -(re[k] - re[FFT_N - k]) * 0.5f;     /* R */
        ml[k] = sqrtf(ar * ar + ai * ai); mr[k] = sqrtf(br * br + bi * bi);
    }
    ml[0] = mr[0] = 0;
    /* onset strength: the rise in log magnitude since the last hop */
    float onset = 0; int loud = 1; float loudv = 0;
    for(int k = 1; k < FFT_N / 2; k++) {
        float m = logf(1e-6f + ml[k] + mr[k]);
        float d = m - prev_logmag[k]; if(d > 0) onset += d;
        prev_logmag[k] = m;
        if(ml[k] + mr[k] > loudv) { loudv = ml[k] + mr[k]; loud = k; }
    }
    /* a beat for the sync page (0.35): an onset well above the recent run of them */
    onset_mean += (onset - onset_mean) * 0.05f;
    if(onset > 2.2f * onset_mean && onset > 0.5f && hops_since_beat > 6) { beats++; hops_since_beat = 0; } else hops_since_beat++;
    if(onset_n < ONSET_HIST) onset_hist[onset_n++] = onset;
    else { memmove(onset_hist, onset_hist + 1, sizeof(float) * (ONSET_HIST - 1)); onset_hist[ONSET_HIST - 1] = onset; }
    pthread_mutex_lock(&an_lock);
    for(int r = 0; r < nrows; r++) {
        float e = 0; int n = 0;
        memset(an.hist[r], 0, sizeof(an.hist[r]));
        for(int k = row_k0[r]; k < row_k1[r]; k++) {
            float m = ml[k] * ml[k] + mr[k] * mr[k], s = ml[k] + mr[k];
            e += m; n++;
            /* where this bin sits between the speakers, into the row's picture (0.31f: once a hop, not per bulb per frame) */
            float pan = s > 1e-6f ? (mr[k] - ml[k]) / s : 0;
            int slot = (int)((pan + 1) * 0.5f * (PAN_SLOTS - 1) + 0.5f);
            if(slot < 0) slot = 0;
            if(slot >= PAN_SLOTS) slot = PAN_SLOTS - 1;
            an.hist[r][slot] += m;
        }
        e = n ? sqrtf(e / n) : 0;
        if(e > band_max[r]) band_max[r] = e; else band_max[r] *= 0.9995f;          /* a slow auto gain per row */
        float v = band_max[r] > 1e-6f ? e / band_max[r] : 0;
        v = logf(1 + 9 * v) / logf(10.0f);                                          /* ears are logarithmic */
        an.band[r] = v > an.band[r] ? v : an.band[r] * 0.85f + v * 0.15f;           /* fast up, slow down */
    }
    for(int i = 0; i < VIZ_BANDS; i++) {
        float e = 0; int n = 0;
        for(int k = sb_k0[i]; k < sb_k0[i + 1] && k < FFT_N / 2; k++) { e += ml[k] * ml[k] + mr[k] * mr[k]; n++; }
        e = n ? sqrtf(e / n) : 0;
        if(e > sband_max[i]) sband_max[i] = e; else sband_max[i] *= 0.9995f;
        float v = sband_max[i] > 1e-6f ? e / sband_max[i] : 0;
        v = logf(1 + 9 * v) / logf(10.0f);
        an.sband[i] = v > an.sband[i] ? v : an.sband[i] * 0.80f + v * 0.20f;
    }
    /* the spectrogram's column: each row the loudest bin in its range (a
     * row narrower than a bin takes the bin it sits in), log-compressed
     * against a slow peak */
    { float colmax = 0; float col[VIZ_SPG_ROWS];
      for(int i = 0; i < VIZ_SPG_ROWS; i++) {
          float k0 = i ? (spg_k[i - 1] + spg_k[i]) / 2 : spg_k[0] * 0.9f, k1 = i < VIZ_SPG_ROWS - 1 ? (spg_k[i] + spg_k[i + 1]) / 2 : spg_k[i] * 1.1f;
          int a = (int)k0, b = (int)ceilf(k1); if(a < 1) a = 1; if(b > FFT_N / 2 - 1) b = FFT_N / 2 - 1; if(b < a) b = a;
          float m = 0; for(int k = a; k <= b; k++) { float v = ml[k] + mr[k]; if(v > m) m = v; }
          col[i] = m; if(m > colmax) colmax = m;
      }
      if(colmax > spg_max) spg_max = colmax; else spg_max *= 0.9997f;
      for(int i = 0; i < VIZ_SPG_ROWS; i++) { float v = spg_max > 1e-6f ? col[i] / spg_max : 0; an.spg[i] = logf(1 + 30 * v) / logf(31.0f); } }
    { float sl = 0, sr2 = 0;
      for(int i = 0; i < HOP; i++) { int k = (head - HOP + i + ring_len) % ring_len; float a = ring[2 * k] / 32768.0f, b = ring[2 * k + 1] / 32768.0f; sl += a * a; sr2 += b * b; }
      an.level_l = sqrtf(sl / HOP); an.level_r = sqrtf(sr2 / HOP); }
    an.loud_bin = loud;
    /* tempo: autocorrelation of the onsets over the last 8 s, 50-200 bpm */
    if(onset_n >= ONSET_HIST / 2) {
        int n = onset_n; float mean = 0; for(int i = 0; i < n; i++) mean += onset_hist[i]; mean /= n;
        float best = 0, bestv = -1, norm = 0;
        for(int i = 0; i < n; i++) norm += (onset_hist[i] - mean) * (onset_hist[i] - mean);
        int lmin = (int)(sr * 60.0f / 200 / HOP), lmax = (int)(sr * 60.0f / 50 / HOP);
        float score[256] = { 0 };
        for(int lag = lmin; lag <= lmax && lag < 256; lag++) {
            float s = 0; for(int i = lag; i < n; i++) s += (onset_hist[i] - mean) * (onset_hist[i - lag] - mean);
            score[lag] = norm > 0 ? s / norm : 0;
            if(score[lag] > bestv) { bestv = score[lag]; best = (float)lag; }
        }
        /* the beat, not the bar: a half (or third) of the best lag that scores nearly as well wins */
        for(int div = 3; div >= 2; div--) {
            int l = (int)(best / div + 0.5f), hit = -1; float hv = 0;
            for(int d = -1; d <= 1; d++) if(l + d >= lmin && l + d <= lmax && score[l + d] > hv) { hv = score[l + d]; hit = l + d; }
            if(hit > 0 && hv >= 0.75f * bestv) { best = (float)hit; bestv = hv; }
        }
        /* steady once locked (0.31f): the lag nearest the last one keeps the
         * beat when it scores nearly as well, so the tempo (and the frame
         * rate with it) doesn't hop between a beat and its double */
        if(an.bpm > 0 && an.bpm_conf >= 0.3f) {
            int l = (int)(60.0f * sr / (an.bpm * HOP) + 0.5f), hit = -1; float hv = 0;
            for(int d = -1; d <= 1; d++) if(l + d >= lmin && l + d <= lmax && score[l + d] > hv) { hv = score[l + d]; hit = l + d; }
            if(hit > 0 && hv >= 0.7f * bestv) { best = (float)hit; bestv = hv; }
        }
        if(best > 0) { an.bpm = 60.0f * sr / (best * HOP); an.bpm_conf = bestv; }
    }
    an.fresh = true;
    pthread_mutex_unlock(&an_lock);
}


static volatile bool stop_flag;
static bool running;
static viz_kind_t kind;

/* ------------------------------------------------------------ the feed */

/* MPD hands the feed over exactly as decoded (0.31g: the fifo has no
 * "format" any more, so MPD does no conversion for it - that conversion ran
 * in a realtime MPD thread at 28 % of a core and starved the DST decoder on
 * the unit's capped clock). The UI reads the format from MPD's status
 * ("dsd64:2", "96000:24:2", "44100:16:2", "48000:f:2") and brings the
 * stream down to 44.1 / 48 kHz 16-bit stereo for the analysis itself, at
 * low priority:
 *   PCM   a windowed-sinc low-pass and a drop to one sample in D (D a power
 *         of two that lands at 44.1 or 48 kHz)
 *   DSD   two steps: a 64-tap FIR done as eight table look-ups per byte
 *         (an 8 KB table that stays in the cache - 0.31h; the one-step
 *         128 KB table of 0.31g missed the cache 11 million times a second
 *         and took 41 % of a core) brings the bits to a sample a byte
 *         (352.8 kHz for DSD64), then the PCM low-pass above takes that to
 *         44.1 kHz; the DSD noise above 20 kHz stays out of the picture */

typedef struct { int rate, bits, ch; bool dsd; int dsd_bytes; } feed_fmt_t;   /* bits: 16 / 24 / 32, -1 float; dsd: rate = bytes/s per channel */
static feed_fmt_t fmt_cur, fmt_next;
static bool fmt_new;
static pthread_mutex_t fmt_lock = PTHREAD_MUTEX_INITIALIZER;

/* the rows' FFT bins follow the analysis rate */
static void set_rate(int rate)
{
    sr = rate;
    delay_samples = (int)((long)delay_ms_set * sr / 1000);
    for(int i = 0; i < nrows; i++) {
        row_k0[i] = (int)ceilf(row_lo(i) * FFT_N / sr); row_k1[i] = (int)ceilf(row_lo(i + 1) * FFT_N / sr);
        if(row_k0[i] < 1) row_k0[i] = 1;
        if(row_k1[i] > FFT_N / 2) row_k1[i] = FFT_N / 2;
        if(row_k1[i] < row_k0[i] + 1) row_k1[i] = row_k0[i] + 1;
    }
    /* the classics' bands: 30 Hz to 18 kHz by log, at least one bin each */
    for(int i = 0; i <= VIZ_BANDS; i++) {
        float f = 30.0f * powf(18000.0f / 30.0f, (float)i / VIZ_BANDS);
        int k = (int)ceilf(f * FFT_N / sr);
        if(k < 1) k = 1;
        if(k > FFT_N / 2) k = FFT_N / 2;
        if(i > 0 && k < sb_k0[i - 1] + 1) k = sb_k0[i - 1] + 1;
        sb_k0[i] = k;
    }
    for(int i = 0; i < VIZ_SPG_ROWS; i++) spg_k[i] = 30.0f * powf(18000.0f / 30.0f, (i + 0.5f) / VIZ_SPG_ROWS) * FFT_N / sr;
}

/* "dsd64:2" / "96000:24:2" / "44100:f:2" -> a format; false when it doesn't parse */
static bool parse_format(const char * audio, feed_fmt_t * f)
{
    memset(f, 0, sizeof(*f));
    if(!audio || !audio[0]) return false;
    if(strncmp(audio, "dsd", 3) == 0) {
        int x = atoi(audio + 3); const char * c = strchr(audio, ':');
        if(x < 64 || x > 1024 || (x & (x - 1))) return false;
        f->dsd = true; f->dsd_bytes = 8 * (x / 64); f->rate = 44100 * 64 / 8 * (x / 64); f->bits = 1; f->ch = c ? atoi(c + 1) : 2;
    }
    else {
        char b[8] = "";
        if(sscanf(audio, "%d:%7[^:]:%d", &f->rate, b, &f->ch) != 3) return false;
        f->bits = b[0] == 'f' ? -1 : atoi(b);
        if(f->bits != 16 && f->bits != 24 && f->bits != 32 && f->bits != -1) return false;
        if(f->rate < 8000 || f->rate > 1000000) return false;
    }
    if(f->ch < 1 || f->ch > 8) return false;
    return true;
}

void viz_set_format(const char * audio)
{
    feed_fmt_t f;
    if(!parse_format(audio, &f)) return;
    pthread_mutex_lock(&fmt_lock);
    if(f.rate != fmt_next.rate || f.bits != fmt_next.bits || f.ch != fmt_next.ch || f.dsd != fmt_next.dsd) { fmt_next = f; fmt_new = true; }
    pthread_mutex_unlock(&fmt_lock);
}

/* the converter's state */
#define CARRY_MAX 4096
static uint8_t carry[CARRY_MAX]; static int carry_n;          /* a partial frame between reads */
static int   frame_bytes;                                      /* one frame of the feed */
static int   decim;                                            /* PCM: input frames per output sample */
static float * taps; static int ntaps;                         /* PCM: the low-pass */
static float * pcm_hist[2]; static int pcm_pos, pcm_count;     /* PCM: the last ntaps samples per channel (a ring, doubled) */
static float * dsd_tab; static int dsd_n;                      /* DSD: dsd_n bytes of FIR as [dsd_n][256] */
static uint8_t * dsd_hist[2]; static int dsd_pos;              /* DSD: the last dsd_n bytes per channel (a ring, doubled) */
static int16_t ring[FFT_N * 2 * 2]; static int ring_head, hop_fill;      /* the analysis input */

static void converter_free(void)
{
    free(taps); taps = NULL; ntaps = 0;
    free(pcm_hist[0]); free(pcm_hist[1]); pcm_hist[0] = pcm_hist[1] = NULL;
    free(dsd_tab); dsd_tab = NULL; dsd_n = 0;
    free(dsd_hist[0]); free(dsd_hist[1]); dsd_hist[0] = dsd_hist[1] = NULL;
    carry_n = 0;
}

/* a windowed sinc (Blackman), cutoff fc as a fraction of the input rate, odd n */
static float * make_lowpass(int n, float fc)
{
    float * h = calloc((size_t)n, sizeof(float)); if(!h) return NULL;
    double sum = 0;
    for(int i = 0; i < n; i++) {
        double x = i - (n - 1) / 2.0, w = 0.42 - 0.5 * cos(2 * M_PI * i / (n - 1)) + 0.08 * cos(4 * M_PI * i / (n - 1));
        double v = x == 0 ? 2 * fc : sin(2 * M_PI * fc * x) / (M_PI * x);
        h[i] = (float)(v * w); sum += h[i];
    }
    for(int i = 0; i < n; i++) h[i] = (float)(h[i] / sum);
    return h;
}

static bool converter_setup(const feed_fmt_t * f)
{
    converter_free();
    if(f->dsd) {
        frame_bytes = f->ch;
        dsd_n = 8;                                                                 /* 64 bits of FIR, one output a byte */
        float * h = make_lowpass(dsd_n * 8, 100000.0f / (f->rate * 8.0f));        /* flat to 20 kHz, -58 dB where the drop to a sample a byte would fold */
        dsd_tab = malloc((size_t)dsd_n * 256 * sizeof(float));
        dsd_hist[0] = calloc((size_t)dsd_n * 2, 1); dsd_hist[1] = calloc((size_t)dsd_n * 2, 1);
        if(!h || !dsd_tab || !dsd_hist[0] || !dsd_hist[1]) { free(h); converter_free(); return false; }
        for(int j = 0; j < dsd_n; j++) for(int b = 0; b < 256; b++) {
            float v = 0;
            for(int i = 0; i < 8; i++) v += h[8 * j + i] * ((b >> (7 - i)) & 1 ? 1.0f : -1.0f);          /* MSB first in time */
            dsd_tab[j * 256 + b] = v;
        }
        free(h);
        dsd_pos = 0;
    }
    /* the PCM path takes it from here: for DSD, from a sample a byte (f->rate is bytes/s a channel) */
    {
        if(!f->dsd) frame_bytes = (f->bits == 16 ? 2 : 4) * f->ch;
        decim = 1; while(f->rate / (decim * 2) >= 44100 && decim < 32) decim *= 2;
        if(decim > 1) {
            ntaps = 16 * decim + 1;
            taps = make_lowpass(ntaps, 0.45f / decim);
            pcm_hist[0] = calloc((size_t)ntaps * 2, sizeof(float)); pcm_hist[1] = calloc((size_t)ntaps * 2, sizeof(float));
            if(!taps || !pcm_hist[0] || !pcm_hist[1]) { converter_free(); return false; }
        }
        pcm_pos = 0; pcm_count = 0;
        set_rate(f->rate / decim);
    }
    hop_fill = 0;
    return true;
}

/* The delay (0.35): the feed runs ahead of what the DAC plays (MPD's output
 * buffer, the DAC's own), so every sample waits here `delay_samples` before
 * the analysis sees it - one setting for every visualizer, remembered per
 * output (0.37: viz-delay-tv / viz-delay-handheld - Ian found the TV path
 * ~200 ms longer than the handheld's; a unit's old viz-delay seeds both).
 * Changing it moves the read point: no reset, a hiccup in the picture at
 * most. */
#define DLINE_MAX (48000 * 6 / 5)                        /* 1.2 s at 48 kHz */
static int16_t dline[DLINE_MAX * 2];
static int dline_pos;
static inline void emit(float l, float r)
{
    l = l > 1 ? 1 : l < -1 ? -1 : l;
    r = r > 1 ? 1 : r < -1 ? -1 : r;
    dline[2 * dline_pos] = (int16_t)(l * 32767.0f); dline[2 * dline_pos + 1] = (int16_t)(r * 32767.0f);
    int d = delay_samples; if(d < 0) d = 0; if(d > DLINE_MAX - 1) d = DLINE_MAX - 1;
    int rp = (dline_pos - d + DLINE_MAX) % DLINE_MAX;
    dline_pos = (dline_pos + 1) % DLINE_MAX;
    ring[2 * ring_head] = dline[2 * rp]; ring[2 * ring_head + 1] = dline[2 * rp + 1];
    ring_head = (ring_head + 1) % (FFT_N * 2);
    if(++hop_fill >= HOP) { hop_fill = 0; analyse_hop(ring, FFT_N * 2, ring_head); }
}

const char * viz_delay_key(void)
{
    static char key[32];
    snprintf(key, sizeof(key), "viz-delay-%s", ui_screen_kind_name());
    return key;
}

int viz_delay_setting(void)
{
    char old[16], d[16];
    config_read("viz-delay", old, sizeof(old), ui_screen.kind == UI_SCREEN_TV ? "630" : "490");   /* Ian's by ear, 21 Oct: HDMI 630, handheld 490 */
    config_read(viz_delay_key(), d, sizeof(d), old);
    int ms = atoi(d); if(ms < 0) ms = 0; if(ms > 1000) ms = 1000;
    return ms;
}

void viz_set_delay_ms(int ms)
{
    if(ms < 0) ms = 0;
    if(ms > 1000) ms = 1000;
    delay_ms_set = ms;
    delay_samples = (int)((long)ms * sr / 1000);
}

/* one sample of the feed at its own rate (a byte's worth for DSD): low-passed and dropped to the analysis rate */
static inline void pcm_push(float l, float r)
{
    if(decim == 1) { emit(l, r); return; }
    pcm_hist[0][pcm_pos] = pcm_hist[0][pcm_pos + ntaps] = l;
    pcm_hist[1][pcm_pos] = pcm_hist[1][pcm_pos + ntaps] = r;
    pcm_pos = (pcm_pos + 1) % ntaps;
    if(++pcm_count >= decim) {
        pcm_count = 0;
        const float * hl = pcm_hist[0] + pcm_pos, * hr = pcm_hist[1] + pcm_pos;
        float sl = 0, sr_ = 0;
        for(int j = 0; j < ntaps; j++) { sl += taps[j] * hl[j]; sr_ += taps[j] * hr[j]; }
        emit(sl, sr_);
    }
}

/* bytes from the feed, any number: whole frames are converted, a partial one kept */
static void feed_bytes(const uint8_t * p, size_t n)
{
    while(n > 0) {
        if(carry_n > 0 || n < (size_t)frame_bytes) {                               /* complete a frame in the carry */
            int take = frame_bytes - carry_n; if((size_t)take > n) take = (int)n;
            memcpy(carry + carry_n, p, (size_t)take); carry_n += take; p += take; n -= (size_t)take;
            if(carry_n < frame_bytes) return;
            uint8_t one[CARRY_MAX]; memcpy(one, carry, (size_t)frame_bytes); carry_n = 0;
            feed_bytes(one, (size_t)frame_bytes);
            continue;
        }
        size_t frames = n / (size_t)frame_bytes;
        const feed_fmt_t * f = &fmt_cur;
        if(f->dsd) {
            for(size_t i = 0; i < frames; i++, p += frame_bytes) {
                uint8_t l = p[0], r = f->ch > 1 ? p[1] : p[0];
                dsd_hist[0][dsd_pos] = dsd_hist[0][dsd_pos + dsd_n] = l;
                dsd_hist[1][dsd_pos] = dsd_hist[1][dsd_pos + dsd_n] = r;
                dsd_pos = (dsd_pos + 1) % dsd_n;
                /* the 64-tap FIR over the last 8 bytes, oldest first: one sample a byte */
                const uint8_t * hl = dsd_hist[0] + dsd_pos, * hr = dsd_hist[1] + dsd_pos;
                float sl = 0, sr_ = 0;
                for(int j = 0; j < dsd_n; j++) { const float * t = dsd_tab + j * 256; sl += t[hl[j]]; sr_ += t[hr[j]]; }
                pcm_push(sl, sr_);
            }
        }
        else {
            for(size_t i = 0; i < frames; i++, p += frame_bytes) {
                float l, r;
                if(f->bits == 16) { int16_t a, b; memcpy(&a, p, 2); memcpy(&b, f->ch > 1 ? p + 2 : p, 2); l = a / 32768.0f; r = b / 32768.0f; }
                else if(f->bits == -1) { memcpy(&l, p, 4); memcpy(&r, f->ch > 1 ? p + 4 : p, 4); }
                else { int32_t a, b; memcpy(&a, p, 4); memcpy(&b, f->ch > 1 ? p + 4 : p, 4);
                       float s = f->bits == 24 ? 1.0f / 8388608.0f : 1.0f / 2147483648.0f; l = a * s; r = b * s; }
                pcm_push(l, r);
            }
        }
        n -= frames * (size_t)frame_bytes;
    }
}

/* ------------------------------------------------------------ the audio thread */

static void * audio_thread(void * arg)
{
    (void)arg;
    if(setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), NICE_AUDIO) < 0) fprintf(stderr, "simpleton-ui: viz: nice failed: %s\n", strerror(errno));
    int fd = open(FIFO_PATH, O_RDONLY | O_NONBLOCK);
    if(fd < 0) { fprintf(stderr, "simpleton-ui: viz: cannot open %s: %s\n", FIFO_PATH, strerror(errno)); return NULL; }
    static uint8_t buf[16384];
    bool have_fmt = false;
    while(!stop_flag) {
        pthread_mutex_lock(&fmt_lock); bool fresh = fmt_new; feed_fmt_t f = fmt_next; fmt_new = false; pthread_mutex_unlock(&fmt_lock);
        if(fresh) {
            /* a new format: set up for it and let what the pipe still holds of the old one go */
            pthread_mutex_lock(&an_lock);
            fmt_cur = f; have_fmt = converter_setup(&f);
            memset(ring, 0, sizeof(ring)); ring_head = 0;
            pthread_mutex_unlock(&an_lock);
            while(read(fd, buf, sizeof(buf)) > 0) ;
            if(have_fmt) {
                if(f.dsd) fprintf(stderr, "simpleton-ui: viz: feed dsd%d:%d -> %d Hz from the bits (64 taps a byte, then %d taps over %d)\n", f.dsd_bytes * 8, f.ch, sr, ntaps, decim);
                else fprintf(stderr, "simpleton-ui: viz: feed %d:%d:%d -> %d Hz%s\n", f.rate, f.bits, f.ch, sr, decim > 1 ? " (low-passed)" : "");
            }
            else fprintf(stderr, "simpleton-ui: viz: feed format not usable\n");
        }
        struct pollfd pfd = { fd, POLLIN, 0 };
        if(poll(&pfd, 1, 200) <= 0) continue;
        ssize_t n = read(fd, buf, sizeof(buf));
        if(n <= 0) { if(n < 0 && (errno == EAGAIN || errno == EINTR)) continue; sleep_s(0.02); continue; }
        if(have_fmt) feed_bytes(buf, (size_t)n);
    }
    close(fd);
    converter_free();
    return NULL;
}

/* ------------------------------------------------------------ the GPU, by hand */

typedef void * EGLDisplay; typedef void * EGLConfig; typedef void * EGLSurface; typedef void * EGLContext;
typedef int32_t EGLint; typedef unsigned EGLBoolean; typedef unsigned GLenum; typedef unsigned GLuint; typedef int GLint; typedef int GLsizei; typedef float GLfloat; typedef unsigned char GLboolean; typedef unsigned GLbitfield;
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_NONE 0x3038
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_VENDOR 0x3053
#define EGL_VERSION 0x3054
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_FLOAT 0x1406
#define GL_TRIANGLES 0x0004
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_BLEND 0x0BE2
#define GL_ONE 1
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_RGBA8 0x8058
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_LINEAR 0x2601
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#define EGL_EXTENSIONS 0x3055
#define DRM_FORMAT_XRGB8888 0x34325258

static EGLDisplay (*p_eglGetDisplay)(void *);
static EGLBoolean (*p_eglInitialize)(EGLDisplay, EGLint *, EGLint *);
static EGLBoolean (*p_eglBindAPI)(unsigned);
static EGLBoolean (*p_eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
static EGLSurface (*p_eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
static EGLContext (*p_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
static EGLBoolean (*p_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static EGLint     (*p_eglGetError)(void);
static const char * (*p_eglQueryString)(EGLDisplay, EGLint);
static GLuint (*p_glCreateShader)(GLenum);
static void (*p_glShaderSource)(GLuint, GLsizei, const char * const *, const GLint *);
static void (*p_glCompileShader)(GLuint);
static void (*p_glGetShaderiv)(GLuint, GLenum, GLint *);
static void (*p_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, char *);
static GLuint (*p_glCreateProgram)(void);
static void (*p_glAttachShader)(GLuint, GLuint);
static void (*p_glLinkProgram)(GLuint);
static void (*p_glGetProgramiv)(GLuint, GLenum, GLint *);
static void (*p_glUseProgram)(GLuint);
static GLint (*p_glGetAttribLocation)(GLuint, const char *);
static void (*p_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
static void (*p_glEnableVertexAttribArray)(GLuint);
static void (*p_glDrawArrays)(GLenum, GLint, GLsizei);
static void (*p_glClear)(GLbitfield);
static void (*p_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
static void (*p_glViewport)(GLint, GLint, GLsizei, GLsizei);
static void (*p_glEnable)(GLenum);
static void (*p_glBlendFunc)(GLenum, GLenum);
static void (*p_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
static void (*p_glFinish)(void);
static GLenum (*p_glGetError)(void);
static const unsigned char * (*p_glGetString)(GLenum);
static void (*p_glGenFramebuffers)(GLsizei, GLuint *);
static void (*p_glBindFramebuffer)(GLenum, GLuint);
static void (*p_glGenRenderbuffers)(GLsizei, GLuint *);
static void (*p_glBindRenderbuffer)(GLenum, GLuint);
static void (*p_glRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
static void (*p_glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
static GLenum (*p_glCheckFramebufferStatus)(GLenum);
static void * (*p_eglGetProcAddress)(const char *);
static void * (*p_eglCreateImageKHR)(EGLDisplay, EGLContext, unsigned, void *, const EGLint *);
static void (*p_glEGLImageTargetRenderbufferStorageOES)(GLenum, void *);

static bool load_gl(void)
{
    void * egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if(!egl) egl = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
    void * gl = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if(!gl) gl = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    if(!egl || !gl) { fprintf(stderr, "viztest: cannot load libEGL / libGLESv2: %s\n", dlerror()); return false; }
#define L(h, n) do { *(void **)&p_##n = dlsym(h, #n); if(!p_##n) { fprintf(stderr, "viztest: no %s\n", #n); return false; } } while(0)
    L(egl, eglGetDisplay); L(egl, eglInitialize); L(egl, eglBindAPI); L(egl, eglChooseConfig); L(egl, eglCreatePbufferSurface);
    L(egl, eglCreateContext); L(egl, eglMakeCurrent); L(egl, eglGetError); L(egl, eglQueryString);
    L(gl, glCreateShader); L(gl, glShaderSource); L(gl, glCompileShader); L(gl, glGetShaderiv); L(gl, glGetShaderInfoLog);
    L(gl, glCreateProgram); L(gl, glAttachShader); L(gl, glLinkProgram); L(gl, glGetProgramiv); L(gl, glUseProgram);
    L(gl, glGetAttribLocation); L(gl, glVertexAttribPointer); L(gl, glEnableVertexAttribArray); L(gl, glDrawArrays);
    L(gl, glClear); L(gl, glClearColor); L(gl, glViewport); L(gl, glEnable); L(gl, glBlendFunc); L(gl, glReadPixels);
    L(gl, glFinish); L(gl, glGetError); L(gl, glGetString);
    L(gl, glGenFramebuffers); L(gl, glBindFramebuffer); L(gl, glGenRenderbuffers); L(gl, glBindRenderbuffer);
    L(gl, glRenderbufferStorage); L(gl, glFramebufferRenderbuffer); L(gl, glCheckFramebufferStatus);
    L(egl, eglGetProcAddress);
#undef L
    *(void **)&p_eglCreateImageKHR = p_eglGetProcAddress("eglCreateImageKHR");
    *(void **)&p_glEGLImageTargetRenderbufferStorageOES = p_eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
    return true;
}


#define EGL_LINUX_DMA_BUF_EXT 0x3270
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define DRM_FORMAT_XRGB8888 0x34325258

static EGLBoolean (*p_eglDestroySurface)(EGLDisplay, EGLSurface);
static EGLBoolean (*p_eglDestroyContext)(EGLDisplay, EGLContext);
static EGLBoolean (*p_eglTerminate)(EGLDisplay);
static void * (*p_eglGetProcAddress)(const char *);
static void * (*p_eglCreateImageKHR)(EGLDisplay, EGLContext, unsigned, void *, const EGLint *);
static EGLBoolean (*p_eglDestroyImageKHR)(EGLDisplay, void *);
static void (*p_glEGLImageTargetRenderbufferStorageOES)(GLenum, void *);
static void (*p_glGenFramebuffers)(GLsizei, GLuint *);
static void (*p_glBindFramebuffer)(GLenum, GLuint);
static void (*p_glGenRenderbuffers)(GLsizei, GLuint *);
static void (*p_glBindRenderbuffer)(GLenum, GLuint);
static void (*p_glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
static GLenum (*p_glCheckFramebufferStatus)(GLenum);
static void (*p_glDeleteFramebuffers)(GLsizei, const GLuint *);
static void (*p_glDeleteRenderbuffers)(GLsizei, const GLuint *);
static void (*p_glDeleteProgram)(GLuint);
static void (*p_glDeleteShader)(GLuint);
static void (*p_glGenTextures)(GLsizei, GLuint *);
static void (*p_glBindTexture)(GLenum, GLuint);
static void (*p_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
static void (*p_glTexParameteri)(GLenum, GLenum, GLint);
static void (*p_glDeleteTextures)(GLsizei, const GLuint *);
static void (*p_glPixelStorei)(GLenum, GLint);
static GLint (*p_glGetUniformLocation)(GLuint, const char *);
static void (*p_glUniform1i)(GLint, GLint);

static bool gl_loaded;
static bool load_gl_more(void)
{
    if(gl_loaded) return true;
    if(!load_gl()) return false;
    void * egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL), * gl = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if(!egl || !gl) return false;
#define L(h, n) do { *(void **)&p_##n = dlsym(h, #n); if(!p_##n) { fprintf(stderr, "simpleton-ui: viz: no %s\n", #n); return false; } } while(0)
    L(egl, eglDestroySurface); L(egl, eglDestroyContext); L(egl, eglTerminate); L(egl, eglGetProcAddress);
    L(gl, glGenFramebuffers); L(gl, glBindFramebuffer); L(gl, glGenRenderbuffers); L(gl, glBindRenderbuffer);
    L(gl, glFramebufferRenderbuffer); L(gl, glCheckFramebufferStatus); L(gl, glDeleteFramebuffers); L(gl, glDeleteRenderbuffers);
    L(gl, glGenTextures); L(gl, glBindTexture); L(gl, glTexImage2D); L(gl, glTexParameteri); L(gl, glDeleteTextures); L(gl, glPixelStorei);
    L(gl, glGetUniformLocation); L(gl, glUniform1i);
    L(gl, glDeleteProgram); L(gl, glDeleteShader);
#undef L
    *(void **)&p_eglCreateImageKHR = p_eglGetProcAddress("eglCreateImageKHR");
    *(void **)&p_eglDestroyImageKHR = p_eglGetProcAddress("eglDestroyImageKHR");
    *(void **)&p_glEGLImageTargetRenderbufferStorageOES = p_eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
    if(!p_eglCreateImageKHR || !p_glEGLImageTargetRenderbufferStorageOES) { fprintf(stderr, "simpleton-ui: viz: no dma-buf import in this EGL\n"); return false; }
    gl_loaded = true;
    return true;
}

static const char * VS =
    "attribute vec2 a_pos; attribute vec2 a_uv; attribute vec4 a_col;\n"
    "varying vec2 v_uv; varying vec4 v_col;\n"
    /* GL counts rows from the bottom, the display from the top: the frame goes straight to the screen, so flip (0.31d) */
    "void main() { v_uv = vec2(a_uv.x, -a_uv.y); v_col = a_col; gl_Position = vec4(a_pos.x, -a_pos.y, 0.0, 1.0); }\n";
static const char * FS =
    "precision mediump float; varying vec2 v_uv; varying vec4 v_col;\n"
    "void main() {\n"
    "  float d = length(v_uv);\n"
    "  float core = 1.0 - smoothstep(0.58, 0.62, d);\n"            /* the bulb: radius 0.6 of the quad */
    "  float glow = exp(-d * d * 4.0) * 0.55;\n"
    "  vec2 h = v_uv - vec2(-0.2, 0.22);\n"
    "  float hl = exp(-dot(h, h) * 30.0) * 0.6;\n"
    "  float b = v_col.a;\n"
    "  vec3 c = v_col.rgb * (core * (0.08 + 0.92 * b) + glow * b) + vec3(hl * b * core);\n"
    "  gl_FragColor = vec4(c, 1.0);\n"
    "}\n";

static GLuint shader(GLenum kind, const char * src)
{
    GLuint s = p_glCreateShader(kind);
    p_glShaderSource(s, 1, &src, NULL);
    p_glCompileShader(s);
    GLint ok = 0; p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if(!ok) { char log[1024]; GLsizei n = 0; p_glGetShaderInfoLog(s, sizeof(log), &n, log); fprintf(stderr, "viztest: shader: %.*s\n", n, log); return 0; }
    return s;
}


/* ------------------------------------------------------------ Soundstage */

/* Ian's ramp (18 Oct 19:46, amended 20:41): the visible spectrum over the
 * audible one - infrared at the bottom for the sub-bass, ultraviolet at the
 * top past 15 kHz, by log frequency. */
/* Soundstage's looks (0.36, Ian's picks from the 21 Oct mock-ups): the
 * original ramp by pitch, Amp 2000 (cyan low, giving way sooner, to a
 * fluorescent pink), Fluoro (highlighter colours by pitch) and Fluoro sweep
 * (the same across the stage, left to right). A bulb's colour is a
 * constant: a look change recolours the vertices before the next frame. */
typedef enum { STAGE_SPECTRUM = 0, STAGE_AMP2000, STAGE_FLUORO, STAGE_FLUORO_SWEEP } stage_look_t;
static stage_look_t stage_look;
static volatile bool recolour;
static const char * const stage_look_codes[4] = { "spectrum", "amp2000", "fluoro", "fluoro-sweep" };
static const uint32_t neon_cs[]   = { 0x4DDFF9, 0x3B93F0, 0x5171EC, 0x7660F3, 0x9456F0, 0xC251EE, 0xFF3BD6, 0xFF1FA8 };
static const uint32_t fluoro_cs[] = { 0xCCFF00, 0x39FF14, 0x00FFC8, 0x00E5FF, 0xFF10F0, 0xFF2BB0, 0xFF6A00 };

static void grad_rgb(const uint32_t * cs, int n, float t, float * r, float * g, float * b)
{
    if(t < 0) t = 0;
    if(t > 1) t = 1;
    float x = t * (n - 1); int i = (int)x; if(i >= n - 1) i = n - 2; float k = x - i;
    uint32_t a = cs[i], c = cs[i + 1];
    *r = (((a >> 16) & 255) * (1 - k) + ((c >> 16) & 255) * k) / 255.0f;
    *g = (((a >> 8) & 255) * (1 - k) + ((c >> 8) & 255) * k) / 255.0f;
    *b = ((a & 255) * (1 - k) + (c & 255) * k) / 255.0f;
}

static void ramp(float f, float * r, float * g, float * b);

/* a bulb's colour for the look: by its row (0 = the bottom) or its stereo position */
static void bulb_colour(int row, float pan, float f, float * r, float * g, float * b)
{
    float t = nrows > 1 ? (float)row / (nrows - 1) : 0;
    switch(stage_look) {
        case STAGE_AMP2000:      grad_rgb(neon_cs, 8, powf(t, 0.7f), r, g, b); break;
        case STAGE_FLUORO:       grad_rgb(fluoro_cs, 7, t, r, g, b); break;
        case STAGE_FLUORO_SWEEP: grad_rgb(fluoro_cs, 7, (pan + 1) / 2, r, g, b); break;
        default:                 ramp(f, r, g, b); break;
    }
}

static void stage_ground(float * r, float * g, float * b)
{
    switch(stage_look) {
        case STAGE_AMP2000: *r = 0x0B / 255.0f; *g = 0x0F / 255.0f; *b = 0x1E / 255.0f; break;
        case STAGE_FLUORO: case STAGE_FLUORO_SWEEP: *r = *g = *b = 0; break;
        default: *r = 0.03f; *g = 0.03f; *b = 0.04f; break;
    }
}

/* The overlay (0.36a): a picture laid over the board - the menu over
 * Soundstage, which LVGL cannot composite itself while the visualizer owns
 * the plane (no spare plane on the TV: takeover). RGBA rows, top first;
 * its place as fractions of the screen. The GPU loop uploads it as a
 * texture when it changes and draws it last, blended. NULL clears it. */
static pthread_mutex_t ov_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t * ov_px; static int ov_w, ov_h; static float ov_x0, ov_y0, ov_x1, ov_y1; static bool ov_dirty;

void viz_set_overlay(uint8_t * rgba, int w, int h, float x0, float y0, float x1, float y1)
{
    pthread_mutex_lock(&ov_lock);
    free(ov_px);
    ov_px = rgba; ov_w = w; ov_h = h; ov_x0 = x0; ov_y0 = y0; ov_x1 = x1; ov_y1 = y1;
    ov_dirty = true;
    pthread_mutex_unlock(&ov_lock);
}

static const char * OVS =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() { gl_Position = vec4(a_pos.x, -a_pos.y, 0.0, 1.0); v_uv = a_uv; }\n";   /* the same flip as the board: the buffer is read bottom-up */
static const char * OFS =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "varying vec2 v_uv;\n"
    "void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n";

void viz_set_stage_look(const char * code)
{
    stage_look_t l = STAGE_SPECTRUM;
    for(int i = 0; i < 4; i++) if(code && strcmp(code, stage_look_codes[i]) == 0) l = (stage_look_t)i;
    stage_look = l;
    recolour = true;
    fprintf(stderr, "simpleton-ui: viz: soundstage look %s\n", stage_look_codes[l]);
}

static void ramp(float f, float * r, float * g, float * b)
{
    static const float fs[] = { 20, 35, 50, 100, 200, 500, 1000, 2000, 3000, 5000, 10000, 15000, 20000 };
    static const uint32_t cs[] = { 0x610000, 0xFF0000, 0xFF7000, 0xFFE000, 0xB8FF00, 0x20FF20, 0x00FFA0, 0x00FFFF, 0x00A8FF, 0x0050FF, 0x2000FF, 0x7900A3, 0x7900A3 };
    int n = 13; int i = 0;
    while(i < n - 2 && f > fs[i + 1]) i++;
    float t = (logf(f) - logf(fs[i])) / (logf(fs[i + 1]) - logf(fs[i]));
    if(t < 0) t = 0;
    if(t > 1) t = 1;
    uint32_t a = cs[i], c = cs[i + 1];
    *r = (((a >> 16) & 255) * (1 - t) + ((c >> 16) & 255) * t) / 255.0f;
    *g = (((a >> 8) & 255) * (1 - t) + ((c >> 8) & 255) * t) / 255.0f;
    *b = ((a & 255) * (1 - t) + (c & 255) * t) / 255.0f;
}

typedef struct { float x, y, r; int row; float pan; float cr, cg, cb; } bulb_t;
static bulb_t bulbs[BULBS_MAX]; static int nbulbs;
static int   VW, VH;                      /* the render size */

/* The pegboard: ACROSS_LOW bulbs across the bottom row, ACROSS_HIGH across
 * the top, the pitch shrinking row by row at one ratio; as many rows as
 * fit the height (14 at 16:9), the whole centred. */
static void build_board(void)
{
    nbulbs = 0;
    set_rate(sr);
    float p0 = (float)VW / ACROSS_LOW, q = 1, total = 0;
    for(nrows = ROWS; nrows > 4; nrows--) {
        q = powf((float)ACROSS_LOW / ACROSS_HIGH, 1.0f / (nrows - 1));
        total = 0; for(int i = 0; i < nrows; i++) total += p0 * powf(q, (float)i);
        if(total <= VH * 0.99f) break;
    }
    float y = VH - (VH - total) / 2;
    for(int i = 0; i < nrows; i++) {
        float pitch = p0 * powf(q, (float)i), d = pitch * 0.86f;
        float yc = y - pitch / 2; y -= pitch;
        int n = (int)(VW / pitch); float off = (VW - n * pitch) / 2 + pitch / 2 + (i % 2 ? pitch / 2 : 0);
        if(i % 2) n--;
        float f = sqrtf(row_lo(i) * row_lo(i + 1));
        row_count[i] = n;
        for(int k = 0; k < n && nbulbs < BULBS_MAX; k++) {
            bulb_t * b = &bulbs[nbulbs++];
            b->x = off + k * pitch; b->y = yc; b->r = d / 2; b->row = i;
            b->pan = (b->x - VW / 2.0f) / (VW / 2.0f);
            bulb_colour(i, b->pan, f, &b->cr, &b->cg, &b->cb);
        }
    }
}

/* each row's spread, the kernel over slot distance: a bulb takes the sound
 * near its own position (0.31f: a table, no expf per bulb per frame) */
static float kern[ROWS][PAN_SLOTS];
static int   bulb_slot[BULBS_MAX];

static void build_kernels(void)
{
    for(int r = 0; r < nrows; r++) {
        float width = 2.6f / (row_count[r] + 1) * (PAN_SLOTS - 1) / 2;             /* in slots */
        for(int d = 0; d < PAN_SLOTS; d++) kern[r][d] = expf(-(d / width) * (d / width));
    }
    for(int i = 0; i < nbulbs; i++) {
        int sl = (int)((bulbs[i].pan + 1) * 0.5f * (PAN_SLOTS - 1) + 0.5f);
        bulb_slot[i] = sl < 0 ? 0 : sl >= PAN_SLOTS ? PAN_SLOTS - 1 : sl;
    }
}

static void light_board(float * bright)
{
    static float band[ROWS], hist[ROWS][PAN_SLOTS], total[ROWS];
    pthread_mutex_lock(&an_lock);                                                   /* a copy, the audio thread barely waits */
    memcpy(band, an.band, sizeof(float) * nrows);
    memcpy(hist, an.hist, sizeof(float) * PAN_SLOTS * nrows);
    pthread_mutex_unlock(&an_lock);
    for(int r = 0; r < nrows; r++) { float t = 0; for(int sl = 0; sl < PAN_SLOTS; sl++) t += hist[r][sl]; total[r] = t; }
    for(int i = 0; i < nbulbs; i++) {
        bulb_t * b = &bulbs[i];
        float e = 0;                                     /* every row in stereo, the bass too (Ian, 18 Oct 22:20) */
        const float * k = kern[b->row], * h = hist[b->row];
        int sb = bulb_slot[i];
        for(int sl = 0; sl < PAN_SLOTS; sl++) e += h[sl] * k[sl > sb ? sl - sb : sb - sl];
        float v = total[b->row] > 1e-9f ? band[b->row] * (e / total[b->row]) * 2.2f : 0;
        if(v > 1) v = 1;
        bright[i] = v > bright[i] ? v : bright[i] * 0.80f + v * 0.20f;                 /* an afterglow */
    }
}

/* ------------------------------------------------------------ the GPU thread */

static pthread_t audio_th, gpu_th;
static display_vizbuf_t vbuf[2];
static int frames_total;

static void * gpu_thread(void * arg)
{
    (void)arg;
    if(setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), NICE_GPU) < 0) fprintf(stderr, "simpleton-ui: viz: nice failed: %s\n", strerror(errno));
    EGLDisplay dpy = p_eglGetDisplay(NULL);
    EGLint maj = 0, min = 0;
    if(!dpy || !p_eglInitialize(dpy, &maj, &min)) { fprintf(stderr, "simpleton-ui: viz: eglInitialize failed (0x%x)\n", p_eglGetError()); return NULL; }
    p_eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig cfg; EGLint ncfg = 0;
    EGLSurface surf = NULL; EGLContext ctx = NULL;
    GLuint vs = 0, fs = 0, prog = 0, ovs = 0, ofs = 0, oprog = 0, otex = 0;
    if(!p_eglChooseConfig(dpy, attr, &cfg, 1, &ncfg) || ncfg < 1) { fprintf(stderr, "simpleton-ui: viz: no EGL config\n"); goto out; }
    { const EGLint pattr[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };            /* a token surface: we draw into our own buffers */
      surf = p_eglCreatePbufferSurface(dpy, cfg, pattr); }
    { const EGLint cattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
      ctx = p_eglCreateContext(dpy, cfg, NULL, cattr); }
    if(!surf || !ctx || !p_eglMakeCurrent(dpy, surf, surf, ctx)) { fprintf(stderr, "simpleton-ui: viz: EGL context failed (0x%x)\n", p_eglGetError()); goto out; }

    vs = shader(GL_VERTEX_SHADER, VS); fs = shader(GL_FRAGMENT_SHADER, FS);
    if(!vs || !fs) goto out;
    prog = p_glCreateProgram(); p_glAttachShader(prog, vs); p_glAttachShader(prog, fs); p_glLinkProgram(prog);
    { GLint ok = 0; p_glGetProgramiv(prog, GL_LINK_STATUS, &ok); if(!ok) { fprintf(stderr, "simpleton-ui: viz: link failed\n"); goto out; } }
    p_glUseProgram(prog);
    GLint a_pos = p_glGetAttribLocation(prog, "a_pos"), a_uv = p_glGetAttribLocation(prog, "a_uv"), a_col = p_glGetAttribLocation(prog, "a_col");
    /* the overlay's program and texture (0.36a) */
    ovs = shader(GL_VERTEX_SHADER, OVS); ofs = shader(GL_FRAGMENT_SHADER, OFS);
    if(!ovs || !ofs) goto out;
    oprog = p_glCreateProgram(); p_glAttachShader(oprog, ovs); p_glAttachShader(oprog, ofs); p_glLinkProgram(oprog);
    { GLint ok = 0; p_glGetProgramiv(oprog, GL_LINK_STATUS, &ok); if(!ok) { fprintf(stderr, "simpleton-ui: viz: overlay link failed\n"); goto out; } }
    GLint o_pos = p_glGetAttribLocation(oprog, "a_pos"), o_uv = p_glGetAttribLocation(oprog, "a_uv"), o_tex = p_glGetUniformLocation(oprog, "u_tex");
    p_glGenTextures(1, &otex); p_glBindTexture(GL_TEXTURE_2D, otex);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    float oq[6][4]; bool ov_on = false;
    p_glUseProgram(prog);

    /* the two display buffers as render targets */
    GLuint fbo[2] = { 0, 0 }, rbo[2] = { 0, 0 }; void * img[2] = { NULL, NULL };
    for(int i = 0; i < 2; i++) {
        const EGLint iattr[] = { EGL_WIDTH, vbuf[i].w, EGL_HEIGHT, vbuf[i].h, EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888,
                                 EGL_DMA_BUF_PLANE0_FD_EXT, vbuf[i].dmabuf_fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0, EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)vbuf[i].pitch, EGL_NONE };
        img[i] = p_eglCreateImageKHR(dpy, NULL, EGL_LINUX_DMA_BUF_EXT, NULL, iattr);
        if(!img[i]) { fprintf(stderr, "simpleton-ui: viz: dma-buf import failed (0x%x)\n", p_eglGetError()); goto out; }
        p_glGenFramebuffers(1, &fbo[i]); p_glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
        p_glGenRenderbuffers(1, &rbo[i]); p_glBindRenderbuffer(GL_RENDERBUFFER, rbo[i]);
        p_glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, img[i]);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo[i]);
        GLenum st = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if(st != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "simpleton-ui: viz: framebuffer %d incomplete (0x%x)\n", i, st); goto out; }
    }
    p_glViewport(0, 0, VW, VH);
    p_glEnable(GL_BLEND); p_glBlendFunc(GL_ONE, GL_ONE);
    { float gr, gg, gb; stage_ground(&gr, &gg, &gb); p_glClearColor(gr, gg, gb, 1); }

    float * verts = malloc((size_t)nbulbs * 6 * 8 * sizeof(float));
    float * bright = calloc((size_t)nbulbs, sizeof(float));
    if(!verts || !bright) { free(verts); free(bright); goto out; }
    /* the geometry and colours never change: fill them once, the brightness per frame */
    for(int i = 0; i < nbulbs; i++) {
        bulb_t * b = &bulbs[i]; float R = b->r * 1.7f;
        float x0 = (b->x - R) / VW * 2 - 1, x1 = (b->x + R) / VW * 2 - 1;
        float y0 = 1 - (b->y + R) / VH * 2, y1 = 1 - (b->y - R) / VH * 2;
        float q[6][4] = { { x0, y0, -1, -1 }, { x1, y0, 1, -1 }, { x1, y1, 1, 1 }, { x0, y0, -1, -1 }, { x1, y1, 1, 1 }, { x0, y1, -1, 1 } };
        for(int v = 0; v < 6; v++) { float * o = verts + ((size_t)i * 6 + v) * 8; o[0] = q[v][0]; o[1] = q[v][1]; o[2] = q[v][2]; o[3] = q[v][3]; o[4] = b->cr; o[5] = b->cg; o[6] = b->cb; o[7] = 0; }
    }
    fprintf(stderr, "simpleton-ui: viz: soundstage %dx%d, %d rows, %d bulbs, GL %s\n", VW, VH, nrows, nbulbs, p_glGetString(GL_RENDERER));

    int cur = 0; double next = now_s(), sec_t = next, draw_ms = 0; int frames = 0; frames_total = 0;
    while(!stop_flag) {
        double f0 = now_s();
        if(recolour) {                                        /* a new look: the bulbs' colours, the ground */
            recolour = false;
            for(int i = 0; i < nbulbs; i++) {
                bulb_t * b = &bulbs[i];
                bulb_colour(b->row, b->pan, sqrtf(row_lo(b->row) * row_lo(b->row + 1)), &b->cr, &b->cg, &b->cb);
                for(int v = 0; v < 6; v++) { float * o = verts + ((size_t)i * 6 + v) * 8; o[4] = b->cr; o[5] = b->cg; o[6] = b->cb; }
            }
            float gr, gg, gb; stage_ground(&gr, &gg, &gb); p_glClearColor(gr, gg, gb, 1);
        }
        light_board(bright);
        for(int i = 0; i < nbulbs; i++) for(int v = 0; v < 6; v++) verts[((size_t)i * 6 + v) * 8 + 7] = bright[i];
        p_glBindFramebuffer(GL_FRAMEBUFFER, fbo[cur]);
        p_glClear(GL_COLOR_BUFFER_BIT);
        p_glVertexAttribPointer((GLuint)a_pos, 2, GL_FLOAT, 0, 8 * sizeof(float), verts);
        p_glVertexAttribPointer((GLuint)a_uv, 2, GL_FLOAT, 0, 8 * sizeof(float), verts + 2);
        p_glVertexAttribPointer((GLuint)a_col, 4, GL_FLOAT, 0, 8 * sizeof(float), verts + 4);
        p_glEnableVertexAttribArray((GLuint)a_pos); p_glEnableVertexAttribArray((GLuint)a_uv); p_glEnableVertexAttribArray((GLuint)a_col);
        p_glDrawArrays(GL_TRIANGLES, 0, nbulbs * 6);
        /* the overlay, if any: uploaded when it changed, drawn over the board */
        pthread_mutex_lock(&ov_lock);
        if(ov_dirty) {
            ov_dirty = false;
            ov_on = ov_px != NULL;
            if(ov_on) {
                p_glBindTexture(GL_TEXTURE_2D, otex);
                p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ov_w, ov_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, ov_px);
                float x0 = ov_x0 * 2 - 1, x1 = ov_x1 * 2 - 1, y0 = 1 - ov_y0 * 2, y1 = 1 - ov_y1 * 2;   /* y0 the top */
                float q[6][4] = { { x0, y0, 0, 0 }, { x1, y0, 1, 0 }, { x1, y1, 1, 1 }, { x0, y0, 0, 0 }, { x1, y1, 1, 1 }, { x0, y1, 0, 1 } };
                memcpy(oq, q, sizeof(oq));
            }
        }
        pthread_mutex_unlock(&ov_lock);
        if(ov_on) {
            p_glUseProgram(oprog);
            p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            p_glBindTexture(GL_TEXTURE_2D, otex); p_glUniform1i(o_tex, 0);
            p_glVertexAttribPointer((GLuint)o_pos, 2, GL_FLOAT, 0, 4 * sizeof(float), &oq[0][0]);
            p_glVertexAttribPointer((GLuint)o_uv, 2, GL_FLOAT, 0, 4 * sizeof(float), &oq[0][2]);
            p_glEnableVertexAttribArray((GLuint)o_pos); p_glEnableVertexAttribArray((GLuint)o_uv);
            p_glDrawArrays(GL_TRIANGLES, 0, 6);
            p_glBlendFunc(GL_ONE, GL_ONE);
            p_glUseProgram(prog);
        }
        p_glFinish();
        display_viz_show(&vbuf[cur]);                       /* on screen; back when the display has it */
        cur ^= 1;
        draw_ms += (now_s() - f0) * 1000; frames++; frames_total++;
        /* the pace: eight frames a beat once the tempo is known, a steady rate otherwise */
        pthread_mutex_lock(&an_lock); float bpm = an.bpm, conf = an.bpm_conf; pthread_mutex_unlock(&an_lock);
        double fps = (bpm > 0 && conf >= 0.3f) ? bpm * FRAMES_PER_BEAT / 60.0 : FPS_STEADY;
        if(fps < FPS_MIN) fps = FPS_MIN;
        if(fps > FPS_MAX) fps = FPS_MAX;
        if(now_s() - sec_t >= 10.0) { fprintf(stderr, "simpleton-ui: viz: %.0f fps, %.1f ms a frame, tempo %.0f bpm (%.2f)\n", frames / (now_s() - sec_t), draw_ms / frames, bpm, conf); frames = 0; draw_ms = 0; sec_t = now_s(); }
        next += 1.0 / fps; double wait = next - now_s();
        if(wait > 0) sleep_s(wait); else next = now_s();
    }
    free(verts); free(bright);
    for(int i = 0; i < 2; i++) { if(fbo[i]) p_glDeleteFramebuffers(1, &fbo[i]); if(rbo[i]) p_glDeleteRenderbuffers(1, &rbo[i]); if(img[i] && p_eglDestroyImageKHR) p_eglDestroyImageKHR(dpy, img[i]); }
out:
    if(otex) p_glDeleteTextures(1, &otex);
    if(oprog) p_glDeleteProgram(oprog);
    if(ovs) p_glDeleteShader(ovs);
    if(ofs) p_glDeleteShader(ofs);
    if(prog) p_glDeleteProgram(prog);
    if(vs) p_glDeleteShader(vs);
    if(fs) p_glDeleteShader(fs);
    p_eglMakeCurrent(dpy, NULL, NULL, NULL);
    if(ctx) p_eglDestroyContext(dpy, ctx);
    if(surf) p_eglDestroySurface(dpy, surf);
    p_eglTerminate(dpy);
    return NULL;
}

/* The feed across a track change (0.31k). With MPD's fifo output on, a new
 * DSD track stalled MPD at its start ("Decoder is too slow" - the DAC got
 * silence: pops at every skip, on plain DSD too; Ian's tests 19 Oct: none
 * with the feed off, none with it on through a song). So the feed goes off
 * just before the UI asks MPD to move, and back on once the new track has
 * been playing a moment; the picture holds still for that moment. */
#define FEED_BACK_MS 1500
static lv_timer_t * feed_timer;
static bool feed_off;

static void feed_back_cb(lv_timer_t * t)
{
    (void)t;
    feed_timer = NULL;
    if(!running || !feed_off) return;
    mpd_status_t st;
    if(mpd_status(&st) && strcmp(st.state, "play") == 0 && st.elapsed < 0.8f) {    /* not yet under way: a little longer */
        feed_timer = lv_timer_create(feed_back_cb, 500, NULL); lv_timer_set_repeat_count(feed_timer, 1); return;
    }
    if(mpd_output_set(FEED_NAME, true)) feed_off = false;
    fprintf(stderr, "simpleton-ui: viz: feed back on\n");
}

static void before_change(void)
{
    if(!running) return;
    if(!feed_off && mpd_output_set(FEED_NAME, false)) { feed_off = true; fprintf(stderr, "simpleton-ui: viz: feed off for the track change\n"); }
    if(feed_timer) lv_timer_delete(feed_timer);                                     /* skips in a row: wait for the last */
    feed_timer = lv_timer_create(feed_back_cb, FEED_BACK_MS, NULL);
    lv_timer_set_repeat_count(feed_timer, 1);
}

/* ------------------------------------------------------------ public */

bool viz_available(void)
{
    return display_viz_available() && load_gl_more();
}

static bool gpu_up;

bool viz_start(viz_kind_t k)
{
    if(running || k == VIZ_NONE) return false;
    bool gpu = k == VIZ_SOUNDSTAGE;
    if(gpu) {
        if(!viz_available()) { fprintf(stderr, "simpleton-ui: viz: no plane or no GPU libraries\n"); return false; }
        /* the render size: half the screen when the plane scales (a TV), else the screen's own */
        bool half = display_viz_scales() && ui_screen.w > 960;
        VW = half ? ui_screen.w / 2 : ui_screen.w;
        VH = half ? ui_screen.h / 2 : ui_screen.h;
        for(int i = 0; i < 2; i++) if(!display_viz_alloc(&vbuf[i], VW, VH)) {
            if(i == 1) display_viz_free(&vbuf[0]);
            fprintf(stderr, "simpleton-ui: viz: no display buffers\n"); return false;
        }
    }
    if(!mpd_output_set(FEED_NAME, true)) {
        fprintf(stderr, "simpleton-ui: viz: the feed (%s) could not be switched on - is it in mpd.conf?\n", FEED_NAME);
        if(gpu) { display_viz_free(&vbuf[0]); display_viz_free(&vbuf[1]); }
        return false;
    }
    kind = k;
    feed_off = false;
    mpd_set_before_change(before_change);
    memset(&an, 0, sizeof(an));
    memset(band_max, 0, sizeof(band_max)); memset(sband_max, 0, sizeof(sband_max)); spg_max = 0;
    memset(dline, 0, sizeof(dline)); dline_pos = 0; onset_mean = 0; hops_since_beat = 0;
    viz_set_delay_ms(viz_delay_setting()); fprintf(stderr, "simpleton-ui: viz: delay %d ms (%s)\n", delay_ms_set, viz_delay_key()); memset(prev_logmag, 0, sizeof(prev_logmag)); onset_n = 0;
    { mpd_status_t st; memset(&fmt_next, 0, sizeof(fmt_next)); fmt_new = false;
      viz_set_format(mpd_status(&st) && st.audio[0] ? st.audio : "44100:16:2"); }       /* whatever plays now; the player's changes follow */
    if(gpu) { char lk[32]; config_read("viz-stage-look", lk, sizeof(lk), "spectrum"); viz_set_stage_look(lk); recolour = false; build_board(); build_kernels(); }
    else set_rate(sr);
    stop_flag = false; running = true; gpu_up = gpu;
    pthread_create(&audio_th, NULL, audio_thread, NULL);
    if(gpu) pthread_create(&gpu_th, NULL, gpu_thread, NULL);
    fprintf(stderr, "simpleton-ui: viz: %s started\n", gpu ? "soundstage" : "the feed (a classic draws it)");
    return true;
}

void viz_stop(void)
{
    if(!running) return;
    stop_flag = true;
    if(gpu_up) pthread_join(gpu_th, NULL);
    pthread_join(audio_th, NULL);
    if(gpu_up) { display_viz_hide(); display_viz_free(&vbuf[0]); display_viz_free(&vbuf[1]); }
    gpu_up = false;
    if(feed_timer) { lv_timer_delete(feed_timer); feed_timer = NULL; }
    if(!feed_off) mpd_output_set(FEED_NAME, false);
    feed_off = false;
    running = false; kind = VIZ_NONE;
    viz_set_overlay(NULL, 0, 0, 0, 0, 0, 0);
    fprintf(stderr, "simpleton-ui: viz: ended (%d frames)\n", frames_total);
}

bool viz_active(void) { return running; }

unsigned viz_beats(void) { return beats; }

int viz_bands(float * out, int max)
{
    int n = max < VIZ_BANDS ? max : VIZ_BANDS;
    pthread_mutex_lock(&an_lock);
    memcpy(out, an.sband, sizeof(float) * (size_t)n);
    pthread_mutex_unlock(&an_lock);
    return running ? n : 0;
}

void viz_levels(float * l, float * r)
{
    pthread_mutex_lock(&an_lock);
    *l = an.level_l; *r = an.level_r;
    pthread_mutex_unlock(&an_lock);
}

int viz_spectrogram(float * out, int max)
{
    int n = max < VIZ_SPG_ROWS ? max : VIZ_SPG_ROWS;
    pthread_mutex_lock(&an_lock);
    memcpy(out, an.spg, sizeof(float) * (size_t)n);
    pthread_mutex_unlock(&an_lock);
    return running ? n : 0;
}

int viz_wave(int16_t * l, int16_t * r, int n)
{
    /* the newest n samples of the analysis ring, oldest first; read without
     * a lock - the audio thread writes one sample at a time, and a frame
     * torn by a sample or two is nothing to a scope */
    if(!running) return 0;
    if(n > FFT_N) n = FFT_N;
    int head = ring_head;
    for(int i = 0; i < n; i++) { int k = (head - n + i + FFT_N * 2) % (FFT_N * 2); l[i] = ring[2 * k]; r[i] = ring[2 * k + 1]; }
    return n;
}



void viz_mpd_changed(void)
{
    if(!running) return;
    mpd_status_t st;
    if(mpd_status(&st) && st.audio[0]) viz_set_format(st.audio);
}
