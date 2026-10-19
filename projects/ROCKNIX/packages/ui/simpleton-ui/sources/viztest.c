/*
 * SimpletonOS - the visualizer test kit (0.30, 18 Oct 2026;
 * visualizers-research.md). A standalone tool for the unit, nothing of it
 * runs in the UI yet. It proves the two foundations before any visualizer
 * is designed:
 *
 *   simpleton-viztest audio <seconds>
 *       reads MPD's second output (the fifo, 44.1 kHz 16-bit stereo) and
 *       logs, once a second: the data rate (a stall shows here), levels,
 *       stereo correlation, the loudest band, a tempo guess.
 *
 *   simpleton-viztest gpu <fps> <seconds> [frame.ppm]
 *       draws ~1000 glowing bulbs at 960x540 on the GPU through EGL/GLES2
 *       (off screen - the UI keeps the display), reads each frame back,
 *       and logs per second: draw and readback times, the frame rate
 *       reached, the SoC temperatures, CPU use. The optional file gets one
 *       frame to look at.
 *
 *   simpleton-viztest stage <fps> <seconds> [frame.ppm]
 *       both at once: the bulbs lit from the audio - a rough first
 *       Soundstage (height = pitch, width = stereo position, bass across
 *       the bottom) - frames dumped to the file every 5 s.
 *
 * EGL and GLES are reached with dlopen() and a few hand-written
 * prototypes, so this builds with no graphics headers or link-time
 * dependency: the image already carries ARM's Mali driver and libEGL /
 * libGLESv2 (checked 18 Oct), nothing is added to it.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FIFO_PATH   "/tmp/simpleton-viz.fifo"
#define SR          44100
#define FFT_N       4096
#define HOP         1024
#define VW          960
#define VH          540
#define ROWS        17
#define BULBS_MAX   1400

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void msleep(double s) { if(s > 0) { struct timespec t = { (time_t)s, (long)((s - (time_t)s) * 1e9) }; nanosleep(&t, NULL); } }

/* ------------------------------------------------------------ the unit's temperatures and CPU */

static void read_temps(char * out, int len)
{
    out[0] = 0;
    for(int z = 0; z < 4; z++) {
        char p[64]; snprintf(p, sizeof(p), "/sys/class/thermal/thermal_zone%d/temp", z);
        FILE * f = fopen(p, "r"); if(!f) break;
        int t = 0; if(fscanf(f, "%d", &t) != 1) t = 0; fclose(f);
        char b[32]; snprintf(b, sizeof(b), "%s%d.%d", z ? " " : "", t / 1000, (t % 1000) / 100);
        strncat(out, b, (size_t)len - strlen(out) - 1);
    }
}

static long long cpu_total_prev, cpu_idle_prev, self_prev;
static void cpu_use(int * sys_pct, int * self_pct)
{
    long long u = 0, n = 0, s = 0, i = 0, w = 0, q = 0, sq = 0;
    FILE * f = fopen("/proc/stat", "r");
    if(f) { if(fscanf(f, "cpu %lld %lld %lld %lld %lld %lld %lld", &u, &n, &s, &i, &w, &q, &sq) < 4) u = 0; fclose(f); }
    long long total = u + n + s + i + w + q + sq;
    long long self = 0;
    f = fopen("/proc/self/stat", "r");
    if(f) { char buf[512]; if(fgets(buf, sizeof(buf), f)) { char * p = strrchr(buf, ')'); long long ut = 0, st = 0; if(p && sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lld %lld", &ut, &st) == 2) self = ut + st; } fclose(f); }
    long long dt = total - cpu_total_prev, di = i - cpu_idle_prev, ds = self - self_prev;
    *sys_pct = dt > 0 ? (int)(100 - di * 100 / dt) : 0;
    *self_pct = dt > 0 ? (int)(ds * 100 * 4 / dt) : 0;          /* of one core (4 cores share `total`) */
    cpu_total_prev = total; cpu_idle_prev = i; self_prev = self;
}

/* ------------------------------------------------------------ analysis */

typedef struct {
    float rms_l, rms_r, peak, corr;         /* the last second */
    float band[ROWS];                        /* energy per row, 0..1 after gain */
    float pan_bin[FFT_N / 2];                /* -1..1 per bin, the last hop */
    float mag_l[FFT_N / 2], mag_r[FFT_N / 2];
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
static float row_lo(int r) { return 25.0f * powf(18000.0f / 25.0f, (float)r / ROWS); }

#define ONSET_HIST 400
static float onset_hist[ONSET_HIST]; static int onset_n;
static float prev_logmag[FFT_N / 2];
static float band_max[ROWS];

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
    if(onset_n < ONSET_HIST) onset_hist[onset_n++] = onset;
    else { memmove(onset_hist, onset_hist + 1, sizeof(float) * (ONSET_HIST - 1)); onset_hist[ONSET_HIST - 1] = onset; }
    pthread_mutex_lock(&an_lock);
    for(int r = 0; r < ROWS; r++) {
        float lo = row_lo(r), hi = row_lo(r + 1); float e = 0; int n = 0;
        for(int k = 1; k < FFT_N / 2; k++) { float f = (float)k * SR / FFT_N; if(f >= lo && f < hi) { e += ml[k] * ml[k] + mr[k] * mr[k]; n++; } }
        e = n ? sqrtf(e / n) : 0;
        if(e > band_max[r]) band_max[r] = e; else band_max[r] *= 0.9995f;          /* a slow auto gain per row */
        float v = band_max[r] > 1e-6f ? e / band_max[r] : 0;
        v = logf(1 + 9 * v) / logf(10.0f);                                          /* ears are logarithmic */
        an.band[r] = v > an.band[r] ? v : an.band[r] * 0.85f + v * 0.15f;           /* fast up, slow down */
    }
    for(int k = 1; k < FFT_N / 2; k++) { float s = ml[k] + mr[k]; an.pan_bin[k] = s > 1e-6f ? (mr[k] - ml[k]) / s : 0; an.mag_l[k] = ml[k]; an.mag_r[k] = mr[k]; }
    an.loud_bin = loud;
    /* tempo: autocorrelation of the onsets over the last 8 s, 50-200 bpm */
    if(onset_n >= ONSET_HIST / 2) {
        int n = onset_n; float mean = 0; for(int i = 0; i < n; i++) mean += onset_hist[i]; mean /= n;
        float best = 0, bestv = -1, norm = 0;
        for(int i = 0; i < n; i++) norm += (onset_hist[i] - mean) * (onset_hist[i] - mean);
        int lmin = (int)(SR * 60.0f / 200 / HOP), lmax = (int)(SR * 60.0f / 50 / HOP);
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
        if(best > 0) { an.bpm = 60.0f * SR / (best * HOP); an.bpm_conf = bestv; }
    }
    an.fresh = true;
    pthread_mutex_unlock(&an_lock);
}

static volatile bool audio_stop;

static void * audio_thread(void * arg)
{
    int seconds = *(int *)arg;
    int fd = open(FIFO_PATH, O_RDONLY);
    if(fd < 0) { fprintf(stderr, "viztest: cannot open %s: %s (is MPD's fifo output enabled and playing?)\n", FIFO_PATH, strerror(errno)); audio_stop = true; return NULL; }
    static int16_t ring[FFT_N * 2 * 2];
    int ring_len = FFT_N * 2, head = 0, since_hop = 0;
    int16_t buf[HOP * 2];
    double t0 = now_s(), sec_t = t0; long bytes = 0; int stalls = 0;
    double sum_l = 0, sum_r = 0, sum_lr = 0, peak = 0; long nsamp = 0;
    while(!audio_stop && now_s() - t0 < seconds) {
        size_t want = sizeof(buf), got = 0;
        double r0 = now_s();
        while(got < want && !audio_stop) {
            struct pollfd pfd = { fd, POLLIN, 0 };
            if(poll(&pfd, 1, 500) <= 0) break;                                     /* nothing for half a second: not playing */
            ssize_t n = read(fd, (char *)buf + got, want - got);
            if(n <= 0) { if(n < 0 && errno == EINTR) continue; break; }
            got += (size_t)n;
        }
        if(got < want) { if(got > 0) stalls++; continue; }
        if(now_s() - r0 > 0.3) stalls++;                                        /* 1024 frames should take 23 ms */
        bytes += (long)got;
        for(int i = 0; i < HOP; i++) {
            float l = buf[2 * i] / 32768.0f, r = buf[2 * i + 1] / 32768.0f;
            sum_l += l * l; sum_r += r * r; sum_lr += l * r; nsamp++;
            if(fabsf(l) > peak) peak = fabsf(l);
            if(fabsf(r) > peak) peak = fabsf(r);
            ring[2 * head] = buf[2 * i]; ring[2 * head + 1] = buf[2 * i + 1]; head = (head + 1) % ring_len;
        }
        since_hop += HOP;
        if(since_hop >= HOP) { since_hop = 0; analyse_hop(ring, ring_len, head); }
        if(now_s() - sec_t >= 1.0) {
            pthread_mutex_lock(&an_lock);
            an.rms_l = nsamp ? sqrtf((float)(sum_l / nsamp)) : 0; an.rms_r = nsamp ? sqrtf((float)(sum_r / nsamp)) : 0;
            an.peak = (float)peak;
            double den = sqrt(sum_l * sum_r); an.corr = den > 0 ? (float)(sum_lr / den) : 0;
            an.bytes_s = bytes; an.stalls = stalls;
            float bpm = an.bpm, conf = an.bpm_conf; int loud = an.loud_bin;
            float rl = an.rms_l, rr = an.rms_r, pk = an.peak, co = an.corr;
            pthread_mutex_unlock(&an_lock);
            char temps[64]; read_temps(temps, sizeof(temps));
            fprintf(stderr, "audio: %ld B/s%s  L %.1f dB  R %.1f dB  peak %.1f dB  corr %+.2f  loudest %d Hz  tempo %.0f bpm (%.2f)  temp %s\n",
                    bytes, stalls ? " STALL" : "", 20 * log10f(rl + 1e-6f), 20 * log10f(rr + 1e-6f), 20 * log10f(pk + 1e-6f), co,
                    loud * SR / FFT_N, bpm, conf, temps);
            bytes = 0; stalls = 0; sum_l = sum_r = sum_lr = 0; peak = 0; nsamp = 0; sec_t = now_s();
        }
    }
    close(fd);
    audio_stop = true;
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

/* ------------------------------------------------------------ a DRM dumb buffer, by hand (the zero-copy target) */

struct dumb_create { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct dumb_map { uint32_t handle, pad; uint64_t offset; };
struct prime_handle { uint32_t handle, flags; int32_t fd; };
#define DRM_IOCTL_MODE_CREATE_DUMB _IOWR('d', 0xB2, struct dumb_create)
#define DRM_IOCTL_MODE_MAP_DUMB    _IOWR('d', 0xB3, struct dumb_map)
#define DRM_IOCTL_PRIME_HANDLE_TO_FD _IOWR('d', 0x2d, struct prime_handle)

static int drm_fd = -1, dumb_fd = -1; static uint32_t dumb_pitch; static uint64_t dumb_size; static void * dumb_map;

static bool dumb_alloc(void)
{
    drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if(drm_fd < 0) { fprintf(stderr, "viztest: cannot open /dev/dri/card0: %s\n", strerror(errno)); return false; }
    struct dumb_create c = { VH, VW, 32, 0, 0, 0, 0 };
    if(ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) < 0) { fprintf(stderr, "viztest: CREATE_DUMB: %s\n", strerror(errno)); return false; }
    dumb_pitch = c.pitch; dumb_size = c.size;
    struct prime_handle ph = { c.handle, O_CLOEXEC | O_RDWR, -1 };
    if(ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) < 0) { fprintf(stderr, "viztest: PRIME_HANDLE_TO_FD: %s\n", strerror(errno)); return false; }
    dumb_fd = ph.fd;
    struct dumb_map m = { c.handle, 0, 0 };
    if(ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &m) < 0) { fprintf(stderr, "viztest: MAP_DUMB: %s\n", strerror(errno)); return false; }
    dumb_map = mmap(NULL, (size_t)dumb_size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, (off_t)m.offset);
    if(dumb_map == MAP_FAILED) { fprintf(stderr, "viztest: mmap dumb: %s\n", strerror(errno)); dumb_map = NULL; return false; }
    fprintf(stderr, "viztest: dumb buffer %dx%d, pitch %u, %llu bytes, dma-buf fd %d\n", VW, VH, dumb_pitch, (unsigned long long)dumb_size, dumb_fd);
    return true;
}

static const char * VS =
    "attribute vec2 a_pos; attribute vec2 a_uv; attribute vec4 a_col;\n"
    "varying vec2 v_uv; varying vec4 v_col;\n"
    "void main() { v_uv = a_uv; v_col = a_col; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
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

/* the pegboard: rows of bulbs shrinking upward (the Soundstage mock-up's geometry) */
typedef struct { float x, y, r; int row; float pan; float h; } bulb_t;
static bulb_t bulbs[BULBS_MAX]; static int nbulbs;
static float row_y[ROWS], row_r[ROWS];
static int   row_k0[ROWS], row_k1[ROWS], row_count[ROWS];   /* the row's FFT bins and how many bulbs it holds */

static void hue_rgb(float h, float * r, float * g, float * b)
{
    float hh = fmodf(h, 360.0f) / 60.0f; int i = (int)hh; float f = hh - i;
    float q = 1 - f, t = f;
    switch(i) { case 0: *r = 1; *g = t; *b = 0; break; case 1: *r = q; *g = 1; *b = 0; break; case 2: *r = 0; *g = 1; *b = t; break;
                case 3: *r = 0; *g = q; *b = 1; break; case 4: *r = t; *g = 0; *b = 1; break; default: *r = 1; *g = 0; *b = q; }
}
/* Ian's ramp: 30 Hz red ... 20 kHz violet, by log frequency */
static float hue_for(float f)
{
    static const float fs[] = { 20, 30, 50, 100, 200, 500, 1500, 2000, 3000, 5000, 10000, 20000 };
    static const float hs[] = { 0, 0, 30, 58, 75, 120, 175, 185, 200, 220, 240, 272 };
    for(int i = 0; i < 11; i++) if(f <= fs[i + 1]) { float t = (logf(f) - logf(fs[i])) / (logf(fs[i + 1]) - logf(fs[i])); return hs[i] + (hs[i + 1] - hs[i]) * (t < 0 ? 0 : t); }
    return 272;
}

static void build_board(void)
{
    float d0 = VH * 0.146f, q = 0.84f, y = VH - 7, total = 0;
    for(int i = 0; i < ROWS; i++) { float d = d0 * powf(q, (float)i); total += d * 1.12f; }
    float s = (VH - 14) / total;
    for(int i = 0; i < ROWS; i++) {
        float d = d0 * powf(q, (float)i) * s, pitch = d * 1.12f;
        row_y[i] = y - pitch / 2; row_r[i] = d / 2; y -= pitch;
        int n = (int)(VW / pitch); float off = (VW - n * pitch) / 2 + pitch / 2 + (i % 2 ? pitch / 2 : 0);
        if(i % 2) n--;
        float f = sqrtf(row_lo(i) * row_lo(i + 1));
        row_k0[i] = (int)ceilf(row_lo(i) * FFT_N / SR); row_k1[i] = (int)ceilf(row_lo(i + 1) * FFT_N / SR);
        if(row_k0[i] < 1) row_k0[i] = 1;
        if(row_k1[i] > FFT_N / 2) row_k1[i] = FFT_N / 2;
        row_count[i] = n;
        for(int k = 0; k < n && nbulbs < BULBS_MAX; k++) {
            bulb_t * b = &bulbs[nbulbs++];
            b->x = off + k * pitch; b->y = row_y[i]; b->r = d / 2; b->row = i;
            b->pan = (b->x - VW / 2.0f) / (VW / 2.0f);
            b->h = hue_for(f);
        }
    }
}

/* brightness per bulb from the analysis (or a test pattern) */
static void light_board(float * bright, bool from_audio, double t)
{
    if(!from_audio) {
        for(int i = 0; i < nbulbs; i++) bright[i] = 0.5f + 0.5f * sinf((float)t * 2.0f + bulbs[i].x * 0.01f + bulbs[i].y * 0.02f);
        return;
    }
    pthread_mutex_lock(&an_lock);
    for(int i = 0; i < nbulbs; i++) {
        bulb_t * b = &bulbs[i];
        float lo = row_lo(b->row), hi = row_lo(b->row + 1);
        float v;
        if(hi <= 120.0f) v = an.band[b->row];                                       /* bass: the whole row */
        else {
            /* the energy of this row's bins that sit at this bulb's stereo position */
            float width = 2.6f / (row_count[b->row] + 1), e = 0, emax = 0;
            for(int k = row_k0[b->row]; k < row_k1[b->row]; k++) {
                float m = an.mag_l[k] * an.mag_l[k] + an.mag_r[k] * an.mag_r[k];
                float d = (an.pan_bin[k] - b->pan) / width;
                e += m * expf(-d * d); emax += m;
            }
            v = emax > 1e-9f ? an.band[b->row] * (e / emax) * 2.2f : 0;
        }
        (void)lo; (void)hi;
        if(v > 1) v = 1;
        bright[i] = v > bright[i] ? v : bright[i] * 0.80f + v * 0.20f;                 /* an afterglow */
    }
    pthread_mutex_unlock(&an_lock);
}

static void dump_ppm(const char * path, const unsigned char * rgba)
{
    FILE * f = fopen(path, "wb"); if(!f) return;
    fprintf(f, "P6\n%d %d\n255\n", VW, VH);
    for(int y = VH - 1; y >= 0; y--) for(int x = 0; x < VW; x++) fwrite(rgba + (y * VW + x) * 4, 1, 3, f);    /* GL is bottom-up */
    fclose(f);
}

static int run_gpu(int fps, int seconds, const char * dump, bool from_audio, const char * target)
{
    bool use_fbo = strcmp(target, "pbuffer") != 0, use_dmabuf = strcmp(target, "dmabuf") == 0;
    if(!load_gl()) return 2;
    EGLDisplay dpy = p_eglGetDisplay(NULL);
    EGLint maj = 0, min = 0;
    if(!dpy || !p_eglInitialize(dpy, &maj, &min)) { fprintf(stderr, "viztest: eglInitialize failed (0x%x)\n", p_eglGetError()); return 2; }
    fprintf(stderr, "viztest: EGL %d.%d, %s, %s\n", maj, min, p_eglQueryString(dpy, EGL_VENDOR), p_eglQueryString(dpy, EGL_VERSION));
    p_eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig cfg; EGLint ncfg = 0;
    if(!p_eglChooseConfig(dpy, attr, &cfg, 1, &ncfg) || ncfg < 1) { fprintf(stderr, "viztest: no EGL config for a pbuffer (0x%x)\n", p_eglGetError()); return 2; }
    const EGLint pattr[] = { EGL_WIDTH, VW, EGL_HEIGHT, VH, EGL_NONE };
    EGLSurface surf = p_eglCreatePbufferSurface(dpy, cfg, pattr);
    const EGLint cattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = p_eglCreateContext(dpy, cfg, NULL, cattr);
    if(!surf || !ctx || !p_eglMakeCurrent(dpy, surf, surf, ctx)) { fprintf(stderr, "viztest: surface/context failed (0x%x)\n", p_eglGetError()); return 2; }
    fprintf(stderr, "viztest: GL %s / %s / %s\n", p_glGetString(GL_VENDOR), p_glGetString(GL_RENDERER), p_glGetString(GL_VERSION));
    GLuint vs = shader(GL_VERTEX_SHADER, VS), fs = shader(GL_FRAGMENT_SHADER, FS);
    if(!vs || !fs) return 2;
    GLuint prog = p_glCreateProgram(); p_glAttachShader(prog, vs); p_glAttachShader(prog, fs); p_glLinkProgram(prog);
    GLint ok = 0; p_glGetProgramiv(prog, GL_LINK_STATUS, &ok); if(!ok) { fprintf(stderr, "viztest: link failed\n"); return 2; }
    p_glUseProgram(prog);
    GLint a_pos = p_glGetAttribLocation(prog, "a_pos"), a_uv = p_glGetAttribLocation(prog, "a_uv"), a_col = p_glGetAttribLocation(prog, "a_col");
    fprintf(stderr, "viztest: EGL extensions: %s\n", p_eglQueryString(dpy, EGL_EXTENSIONS));
    GLuint fbo = 0, rbo = 0;
    if(use_fbo) {
        /* draw into a renderbuffer: either the GPU's own (then glReadPixels), or a
         * DRM dumb buffer imported as a dma-buf (then the CPU - or the display - reads
         * the buffer itself: no readback at all) */
        p_glGenFramebuffers(1, &fbo); p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        p_glGenRenderbuffers(1, &rbo); p_glBindRenderbuffer(GL_RENDERBUFFER, rbo);
        if(use_dmabuf) {
            if(!p_eglCreateImageKHR || !p_glEGLImageTargetRenderbufferStorageOES) { fprintf(stderr, "viztest: no dma-buf import on this EGL\n"); return 2; }
            if(!dumb_alloc()) return 2;
            const EGLint iattr[] = { EGL_WIDTH, VW, EGL_HEIGHT, VH, EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888,
                                     EGL_DMA_BUF_PLANE0_FD_EXT, dumb_fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0, EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)dumb_pitch, EGL_NONE };
            void * img = p_eglCreateImageKHR(dpy, NULL, EGL_LINUX_DMA_BUF_EXT, NULL, iattr);
            if(!img) { fprintf(stderr, "viztest: eglCreateImageKHR(dma-buf) failed (0x%x)\n", p_eglGetError()); return 2; }
            p_glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, img);
        }
        else p_glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, VW, VH);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);
        GLenum st = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if(st != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "viztest: framebuffer incomplete (0x%x, GL 0x%x)\n", st, p_glGetError()); return 2; }
        fprintf(stderr, "viztest: drawing into %s\n", use_dmabuf ? "a DRM dumb buffer (dma-buf)" : "a renderbuffer");
    }
    p_glViewport(0, 0, VW, VH);
    p_glEnable(GL_BLEND); p_glBlendFunc(GL_ONE, GL_ONE);
    p_glClearColor(0.03f, 0.03f, 0.04f, 1);

    build_board();
    fprintf(stderr, "viztest: %d bulbs in %d rows, %dx%d, %d fps for %d s\n", nbulbs, ROWS, VW, VH, fps, seconds);
    float * verts = malloc((size_t)nbulbs * 6 * 8 * sizeof(float));
    float * bright = calloc((size_t)nbulbs, sizeof(float));
    unsigned char * pixels = malloc((size_t)VW * VH * 4);
    if(!verts || !bright || !pixels) return 2;

    double t0 = now_s(), sec_t = t0, draw_ms = 0, read_ms = 0, build_ms = 0; int frames = 0, total_frames = 0;
    double period = 1.0 / fps, next = t0; int dumped = 0;
    cpu_use(&(int){0}, &(int){0});
    while(now_s() - t0 < seconds && !(from_audio && audio_stop)) {
        double f0 = now_s();
        light_board(bright, from_audio, f0 - t0);
        /* two triangles per bulb, the quad 1.7x the bulb for its glow */
        for(int i = 0; i < nbulbs; i++) {
            bulb_t * b = &bulbs[i]; float R = b->r * 1.7f;
            float cr, cg, cb; hue_rgb(b->h, &cr, &cg, &cb);
            float x0 = (b->x - R) / VW * 2 - 1, x1 = (b->x + R) / VW * 2 - 1;
            float y0 = 1 - (b->y + R) / VH * 2, y1 = 1 - (b->y - R) / VH * 2;
            float q[6][4] = { { x0, y0, -1, -1 }, { x1, y0, 1, -1 }, { x1, y1, 1, 1 }, { x0, y0, -1, -1 }, { x1, y1, 1, 1 }, { x0, y1, -1, 1 } };
            for(int v = 0; v < 6; v++) { float * o = verts + ((size_t)i * 6 + v) * 8; o[0] = q[v][0]; o[1] = q[v][1]; o[2] = q[v][2]; o[3] = q[v][3]; o[4] = cr; o[5] = cg; o[6] = cb; o[7] = bright[i]; }
        }
        double f1 = now_s();
        p_glClear(GL_COLOR_BUFFER_BIT);
        p_glVertexAttribPointer((GLuint)a_pos, 2, GL_FLOAT, 0, 8 * sizeof(float), verts);
        p_glVertexAttribPointer((GLuint)a_uv, 2, GL_FLOAT, 0, 8 * sizeof(float), verts + 2);
        p_glVertexAttribPointer((GLuint)a_col, 4, GL_FLOAT, 0, 8 * sizeof(float), verts + 4);
        p_glEnableVertexAttribArray((GLuint)a_pos); p_glEnableVertexAttribArray((GLuint)a_uv); p_glEnableVertexAttribArray((GLuint)a_col);
        p_glDrawArrays(GL_TRIANGLES, 0, nbulbs * 6);
        p_glFinish();
        double f2 = now_s();
        if(use_dmabuf) {
            /* the frame is already in CPU-visible memory: copy it out (the display could scan it out instead) */
            for(int y = 0; y < VH; y++) {
                const unsigned char * src = (const unsigned char *)dumb_map + (size_t)y * dumb_pitch;
                unsigned char * dst = pixels + (size_t)(VH - 1 - y) * VW * 4;                  /* flip to GL's bottom-up for the dump */
                for(int x = 0; x < VW; x++) { dst[x * 4] = src[x * 4 + 2]; dst[x * 4 + 1] = src[x * 4 + 1]; dst[x * 4 + 2] = src[x * 4]; dst[x * 4 + 3] = 255; }
            }
        }
        else p_glReadPixels(0, 0, VW, VH, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        double f3 = now_s();
        build_ms += (f1 - f0) * 1000; draw_ms += (f2 - f1) * 1000; read_ms += (f3 - f2) * 1000; frames++; total_frames++;
        GLenum err = p_glGetError(); if(err) fprintf(stderr, "viztest: GL error 0x%x\n", err);
        if(dump && (total_frames == fps || (from_audio && total_frames % (fps * 5) == 0))) { dump_ppm(dump, pixels); dumped++; }
        if(now_s() - sec_t >= 1.0) {
            char temps[64]; read_temps(temps, sizeof(temps)); int sys = 0, self = 0; cpu_use(&sys, &self);
            fprintf(stderr, "gpu: %d fps  build %.1f ms  draw %.1f ms  readback %.1f ms  temp %s  cpu %d%% (this %d%% of a core)\n",
                    frames, build_ms / frames, draw_ms / frames, read_ms / frames, temps, sys, self);
            frames = 0; build_ms = draw_ms = read_ms = 0; sec_t = now_s();
        }
        next += period; double wait = next - now_s();
        if(wait > 0) msleep(wait); else next = now_s();
    }
    if(dump) fprintf(stderr, "viztest: %d frame%s written to %s\n", dumped, dumped == 1 ? "" : "s", dump);
    free(verts); free(bright); free(pixels);
    return 0;
}

int main(int argc, char ** argv)
{
    if(argc < 2) { fprintf(stderr, "usage: simpleton-viztest audio <seconds> | gpu <fps> <seconds> [frame.ppm] [pbuffer|fbo|dmabuf] | stage <fps> <seconds> [frame.ppm] [pbuffer|fbo|dmabuf]\n"); return 1; }
    if(strcmp(argv[1], "audio") == 0) {
        int seconds = argc > 2 ? atoi(argv[2]) : 20;
        pthread_t th; pthread_create(&th, NULL, audio_thread, &seconds); pthread_join(th, NULL);
        return 0;
    }
    int fps = argc > 2 ? atoi(argv[2]) : 30, seconds = argc > 3 ? atoi(argv[3]) : 20;
    const char * dump = argc > 4 ? argv[4] : NULL;
    const char * target = argc > 5 ? argv[5] : "dmabuf";                /* pbuffer | fbo | dmabuf */
    if(fps < 1) fps = 1;
    if(strcmp(argv[1], "gpu") == 0) return run_gpu(fps, seconds, dump, false, target);
    if(strcmp(argv[1], "stage") == 0) {
        static int secs; secs = seconds + 2;
        pthread_t th; pthread_create(&th, NULL, audio_thread, &secs);
        msleep(0.5);
        int rc = run_gpu(fps, seconds, dump, true, target);
        audio_stop = true; pthread_join(th, NULL);
        return rc;
    }
    fprintf(stderr, "viztest: unknown mode %s\n", argv[1]);
    return 1;
}
