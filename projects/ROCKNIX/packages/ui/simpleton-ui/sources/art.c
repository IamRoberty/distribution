/*
 * SimpletonOS UI - album art loader (implementation). See art.h.
 *
 * Decoders: libjpeg-turbo and libpng, straight from the ROCKNIX tree. Not
 * LVGL's built-in TJpgDec: it can't read progressive JPEGs, which a lot of
 * downloaded cover art is, and those albums would show a placeholder for
 * no visible reason. libjpeg-turbo also scales *during* decode (DCT
 * scaling, 1/2 1/4 1/8), so a 3000px cover is never fully decoded.
 *
 * Output is XRGB8888 (the panel's native format, LV_COLOR_DEPTH 32) at the
 * final on-screen size, so LVGL blits it without transforming.
 */
#define _GNU_SOURCE          /* asprintf */
#include "art.h"
#include "mpdc.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>              /* before jpeglib.h: it uses FILE unguarded */
#include <stdlib.h>
#include <jpeglib.h>
#include <png.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* MPD's music_directory. MPD only answers the `config` command on a local
 * socket, not TCP, so this is fixed here and matches mpd.conf. */
#define MUSIC_DIR "/storage/music"

#define MAX_IMAGE_BYTES (64u << 20)

/* ---------------------------------------------------------------- state */

static int box_size = 720;

static pthread_t       worker;
static pthread_mutex_t mu   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv   = PTHREAD_COND_INITIALIZER;

/* request slot (latest wins) */
static char *   req_uri;
static unsigned req_gen;

/* result slot */
static bool          res_ready;
static bool          res_found;
static unsigned      res_gen;
static art_result_t  res;

/* ------------------------------------------------------------- helpers */

static bool has_ext(const char * name, const char * const * exts)
{
    const char * dot = strrchr(name, '.');
    if(!dot) return false;
    for(int i = 0; exts[i]; i++) if(strcasecmp(dot + 1, exts[i]) == 0) return true;
    return false;
}

static const char * const image_exts[] = { "jpg", "jpeg", "png", NULL };

/* Case-insensitive search for a container marker (".iso/" or ".cue/") in a
 * URI; returns a pointer to the '.' or NULL. */
static const char * find_container(const char * uri)
{
    for(const char * p = uri; *p; p++) {
        if(*p != '.') continue;
        if((strncasecmp(p, ".iso/", 5) == 0) || (strncasecmp(p, ".cue/", 5) == 0)) return p;
    }
    return NULL;
}

/* The directory on disk that holds the album: the track's folder, or for a
 * container track the folder holding the container. */
static void album_dir(const char * uri, char * out, size_t len)
{
    const char * end = find_container(uri);
    if(!end) end = uri + strlen(uri);
    /* walk back to the slash before the file/container name */
    const char * slash = NULL;
    for(const char * p = uri; p < end; p++) if(*p == '/') slash = p;
    size_t n = slash ? (size_t)(slash - uri) : 0;
    snprintf(out, len, "%s/%.*s", MUSIC_DIR, (int)n, uri);
}

static uint8_t * read_file(const char * path, size_t * len)
{
    *len = 0;
    FILE * f = fopen(path, "rb");
    if(!f) return NULL;
    struct stat st;
    if(fstat(fileno(f), &st) != 0 || st.st_size <= 0 || (size_t)st.st_size > MAX_IMAGE_BYTES) { fclose(f); return NULL; }
    uint8_t * buf = malloc((size_t)st.st_size);
    if(buf && fread(buf, 1, (size_t)st.st_size, f) == (size_t)st.st_size) *len = (size_t)st.st_size;
    else { free(buf); buf = NULL; }
    fclose(f);
    return buf;
}

/* ------------------------------------------------------ folder lookup */

static int alpha_cmp(const struct dirent ** a, const struct dirent ** b)
{
    return strcasecmp((*a)->d_name, (*b)->d_name);
}

/* Pick the best image directly in `dir` by well-known stem, else NULL. */
static char * pick_named(const char * dir)
{
    static const char * const stems[] = { "cover", "folder", "front", "album", "albumart", NULL };
    struct dirent ** ents;
    int n = scandir(dir, &ents, NULL, alpha_cmp);
    if(n < 0) return NULL;
    char * best = NULL;
    int best_rank = 99;
    for(int i = 0; i < n; i++) {
        const char * name = ents[i]->d_name;
        if(has_ext(name, image_exts)) {
            char stem[256];
            snprintf(stem, sizeof(stem), "%s", name);
            char * dot = strrchr(stem, '.');
            if(dot) *dot = 0;
            for(int r = 0; stems[r]; r++) {
                if(strcasecmp(stem, stems[r]) == 0 && r < best_rank) {
                    best_rank = r;
                    free(best);
                    if(asprintf(&best, "%s/%s", dir, name) < 0) best = NULL;
                }
            }
        }
        free(ents[i]);
    }
    free(ents);
    return best;
}

/* Pick the front-most image inside an artwork subfolder, else NULL. */
static char * pick_in_subfolder(const char * dir)
{
    static const char * const subs[] = { "artwork", "scans", "covers", "cover", "art", "images", "scan", NULL };
    struct dirent ** ents;
    int n = scandir(dir, &ents, NULL, alpha_cmp);
    if(n < 0) return NULL;
    char * best = NULL;
    for(int i = 0; i < n && !best; i++) {
        const char * name = ents[i]->d_name;
        bool is_sub = false;
        for(int s = 0; subs[s]; s++) if(strcasecmp(name, subs[s]) == 0) is_sub = true;
        if(!is_sub) continue;

        char sub[1600];
        snprintf(sub, sizeof(sub), "%s/%s", dir, name);
        struct dirent ** files;
        int m = scandir(sub, &files, NULL, alpha_cmp);
        if(m < 0) continue;
        char * first = NULL, * front = NULL, * cover = NULL;
        for(int j = 0; j < m; j++) {
            const char * fn = files[j]->d_name;
            if(has_ext(fn, image_exts)) {
                char lower[256];
                size_t k;
                for(k = 0; fn[k] && k < sizeof(lower) - 1; k++) lower[k] = (char)tolower((unsigned char)fn[k]);
                lower[k] = 0;
                if(!first && asprintf(&first, "%s/%s", sub, fn) < 0) first = NULL;
                if(!front && strstr(lower, "front") && asprintf(&front, "%s/%s", sub, fn) < 0) front = NULL;
                if(!cover && strstr(lower, "cover") && asprintf(&cover, "%s/%s", sub, fn) < 0) cover = NULL;
            }
            free(files[j]);
        }
        free(files);
        best = front ? front : (cover ? cover : first);
        if(best != front) free(front);
        if(best != cover) free(cover);
        if(best != first) free(first);
    }
    for(int i = 0; i < n; i++) free(ents[i]);
    free(ents);
    return best;
}

/* ------------------------------------------------------------- decode */

typedef struct { uint8_t * rgb; int w, h; int src_w, src_h; } rgb_image_t;

struct jerr { struct jpeg_error_mgr pub; jmp_buf jb; };
static void jerr_exit(j_common_ptr c) { longjmp(((struct jerr *)c->err)->jb, 1); }
static void jerr_msg(j_common_ptr c) { (void)c; }   /* quiet */

static bool decode_jpeg(const uint8_t * data, size_t len, rgb_image_t * out)
{
    struct jpeg_decompress_struct cinfo;
    struct jerr err;
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = jerr_exit;
    err.pub.output_message = jerr_msg;
    uint8_t * rgb = NULL;

    if(setjmp(err.jb)) { jpeg_destroy_decompress(&cinfo); free(rgb); return false; }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (unsigned char *)data, (unsigned long)len);
    if(jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) { jpeg_destroy_decompress(&cinfo); return false; }
    out->src_w = (int)cinfo.image_width;
    out->src_h = (int)cinfo.image_height;

    /* DCT scaling: shrink as far as possible while staying >= the box on
     * the longer edge, so the resampler below only ever works on <= 2x. */
    int longest = out->src_w > out->src_h ? out->src_w : out->src_h;
    unsigned denom = 1;
    while(denom < 8 && longest / (int)(denom * 2) >= box_size) denom *= 2;
    cinfo.scale_num = 1;
    cinfo.scale_denom = denom;
    cinfo.out_color_space = JCS_RGB;
    cinfo.dct_method = JDCT_IFAST;
    jpeg_calc_output_dimensions(&cinfo);
    jpeg_start_decompress(&cinfo);

    int w = (int)cinfo.output_width, h = (int)cinfo.output_height;
    if(cinfo.output_components != 3 || w <= 0 || h <= 0) longjmp(err.jb, 1);
    rgb = malloc((size_t)w * (size_t)h * 3);
    if(!rgb) longjmp(err.jb, 1);
    while(cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row = rgb + (size_t)cinfo.output_scanline * (size_t)w * 3;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    out->rgb = rgb;
    out->w = w;
    out->h = h;
    return true;
}

typedef struct { const uint8_t * p; size_t len, off; } png_mem_t;
static void png_mem_read(png_structp png, png_bytep dst, png_size_t n)
{
    png_mem_t * m = png_get_io_ptr(png);
    if(m->off + n > m->len) png_error(png, "eof");
    memcpy(dst, m->p + m->off, n);
    m->off += n;
}

static bool decode_png(const uint8_t * data, size_t len, rgb_image_t * out)
{
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if(!png) return false;
    png_infop info = png_create_info_struct(png);
    if(!info) { png_destroy_read_struct(&png, NULL, NULL); return false; }
    uint8_t * rgb = NULL;
    png_bytep * rows = NULL;
    if(setjmp(png_jmpbuf(png))) { png_destroy_read_struct(&png, &info, NULL); free(rgb); free(rows); return false; }

    png_mem_t m = { data, len, 0 };
    png_set_read_fn(png, &m, png_mem_read);
    png_read_info(png, info);

    png_uint_32 w = png_get_image_width(png, info), h = png_get_image_height(png, info);
    int color = png_get_color_type(png, info), depth = png_get_bit_depth(png, info);
    if(w == 0 || h == 0 || w > 16384 || h > 16384) longjmp(png_jmpbuf(png), 1);

    /* normalise everything to 8-bit RGB */
    if(color == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if(color == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if(png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if(depth == 16) png_set_strip_16(png);
    if(color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    png_set_strip_alpha(png);
    png_set_interlace_handling(png);
    png_read_update_info(png, info);
    if(png_get_channels(png, info) != 3) longjmp(png_jmpbuf(png), 1);

    rgb = malloc((size_t)w * (size_t)h * 3);
    rows = malloc(sizeof(png_bytep) * h);
    if(!rgb || !rows) longjmp(png_jmpbuf(png), 1);
    for(png_uint_32 y = 0; y < h; y++) rows[y] = rgb + (size_t)y * w * 3;
    png_read_image(png, rows);
    png_destroy_read_struct(&png, &info, NULL);
    free(rows);
    out->rgb = rgb;
    out->w = out->src_w = (int)w;
    out->h = out->src_h = (int)h;
    return true;
}

static bool decode_any(const uint8_t * data, size_t len, rgb_image_t * out)
{
    memset(out, 0, sizeof(*out));
    if(len > 3 && data[0] == 0xFF && data[1] == 0xD8) return decode_jpeg(data, len, out);
    if(len > 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) return decode_png(data, len, out);
    return false;
}

/* Fit `src` inside box x box, producing XRGB8888. Box filter when shrinking
 * (no aliasing on a 2x reduction), bilinear when enlarging. */
static uint8_t * resample_fit(const rgb_image_t * src, int * out_w, int * out_h)
{
    double scale = (double)box_size / (src->w > src->h ? src->w : src->h);
    int dw = (int)lround(src->w * scale), dh = (int)lround(src->h * scale);
    if(dw < 1) dw = 1;
    if(dh < 1) dh = 1;
    uint32_t * dst = malloc((size_t)dw * (size_t)dh * 4);
    if(!dst) return NULL;

    const uint8_t * s = src->rgb;
    int sw = src->w, sh = src->h;

    if(sw >= dw && sh >= dh) {
        for(int y = 0; y < dh; y++) {
            int y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
            if(y1 <= y0) y1 = y0 + 1;
            for(int x = 0; x < dw; x++) {
                int x0 = x * sw / dw, x1 = (x + 1) * sw / dw;
                if(x1 <= x0) x1 = x0 + 1;
                unsigned r = 0, g = 0, b = 0, n = 0;
                for(int yy = y0; yy < y1; yy++) {
                    const uint8_t * row = s + ((size_t)yy * sw + x0) * 3;
                    for(int xx = x0; xx < x1; xx++, row += 3) { r += row[0]; g += row[1]; b += row[2]; n++; }
                }
                dst[(size_t)y * dw + x] = 0xFF000000u | ((r / n) << 16) | ((g / n) << 8) | (b / n);
            }
        }
    }
    else {
        for(int y = 0; y < dh; y++) {
            double fy = (y + 0.5) * sh / dh - 0.5;
            int y0 = (int)floor(fy); double ty = fy - y0;
            if(y0 < 0) { y0 = 0; ty = 0; }
            int y1 = y0 + 1 < sh ? y0 + 1 : y0;
            for(int x = 0; x < dw; x++) {
                double fx = (x + 0.5) * sw / dw - 0.5;
                int x0 = (int)floor(fx); double tx = fx - x0;
                if(x0 < 0) { x0 = 0; tx = 0; }
                int x1 = x0 + 1 < sw ? x0 + 1 : x0;
                const uint8_t * p00 = s + ((size_t)y0 * sw + x0) * 3, * p01 = s + ((size_t)y0 * sw + x1) * 3;
                const uint8_t * p10 = s + ((size_t)y1 * sw + x0) * 3, * p11 = s + ((size_t)y1 * sw + x1) * 3;
                unsigned c[3];
                for(int k = 0; k < 3; k++) {
                    double top = p00[k] + (p01[k] - p00[k]) * tx;
                    double bot = p10[k] + (p11[k] - p10[k]) * tx;
                    c[k] = (unsigned)lround(top + (bot - top) * ty);
                }
                dst[(size_t)y * dw + x] = 0xFF000000u | (c[0] << 16) | (c[1] << 8) | c[2];
            }
        }
    }
    *out_w = dw;
    *out_h = dh;
    return (uint8_t *)dst;
}

/* ------------------------------------------------------------- lookup */

/* Embedded art. For a CUE track, MPD indexes the sheet's virtual tracks and
 * won't read tags through them, so ask about the audio file beside the
 * sheet instead (same stem, common lossless extensions). */
static uint8_t * fetch_embedded(const char * uri, size_t * len, char * source, size_t slen)
{
    uint8_t * data;
    const char * cont = find_container(uri);
    if(cont && strncasecmp(cont, ".cue/", 5) == 0) {
        static const char * const exts[] = { "flac", "ape", "wv", "wav", "tak", NULL };
        for(int i = 0; exts[i]; i++) {
            char alt[1024];
            snprintf(alt, sizeof(alt), "%.*s.%s", (int)(cont - uri), uri, exts[i]);
            if(mpd_readpicture(alt, &data, len)) { snprintf(source, slen, "embedded (%s)", exts[i]); return data; }
        }
        return NULL;
    }
    if(cont) return NULL;                /* ISO: nothing embedded to read */
    if(mpd_readpicture(uri, &data, len)) { snprintf(source, slen, "embedded"); return data; }
    return NULL;
}

static uint8_t * fetch_folder(const char * uri, size_t * len, char * source, size_t slen)
{
    char dir[1200];
    album_dir(uri, dir, sizeof(dir));
    char * path = pick_named(dir);
    if(!path) path = pick_in_subfolder(dir);
    if(!path) return NULL;
    uint8_t * data = read_file(path, len);
    const char * base = strrchr(path, '/');
    snprintf(source, slen, "%s", base ? base + 1 : path);
    free(path);
    return data;
}

/* FNV-1a over the encoded bytes: cheap identity check so ten tracks of one
 * album with the same embedded picture decode it once. */
static uint64_t fnv(const uint8_t * p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for(size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

/* --------------------------------------------------------------- worker */

static void * worker_main(void * arg)
{
    (void)arg;
    /* cache of the last decode, keyed by the encoded bytes' hash */
    uint64_t cache_key = 0;
    art_result_t cache = { 0 };

    for(;;) {
        pthread_mutex_lock(&mu);
        while(!req_uri) pthread_cond_wait(&cv, &mu);
        char * uri = req_uri;
        unsigned gen = req_gen;
        req_uri = NULL;
        pthread_mutex_unlock(&mu);

        art_result_t r = { 0 };
        bool found = false;
        size_t len = 0;
        uint8_t * data = fetch_embedded(uri, &len, r.source, sizeof(r.source));
        if(!data) data = fetch_folder(uri, &len, r.source, sizeof(r.source));

        if(data) {
            uint64_t key = fnv(data, len);
            if(cache.pixels && key == cache_key) {
                r = cache;
                r.pixels = malloc((size_t)cache.w * (size_t)cache.h * 4);
                if(r.pixels) { memcpy(r.pixels, cache.pixels, (size_t)cache.w * (size_t)cache.h * 4); found = true; }
            }
            else {
                rgb_image_t img;
                if(decode_any(data, len, &img)) {
                    r.pixels = resample_fit(&img, &r.w, &r.h);
                    r.src_w = img.src_w;
                    r.src_h = img.src_h;
                    free(img.rgb);
                    if(r.pixels) {
                        found = true;
                        /* keep a copy for the next track of this album */
                        free(cache.pixels);
                        cache = r;
                        cache.pixels = malloc((size_t)r.w * (size_t)r.h * 4);
                        if(cache.pixels) { memcpy(cache.pixels, r.pixels, (size_t)r.w * (size_t)r.h * 4); cache_key = key; }
                    }
                }
                else fprintf(stderr, "simpleton-ui: art: could not decode %s (%zu bytes) for %s\n", r.source, len, uri);
            }
            free(data);
        }
        if(found) fprintf(stderr, "simpleton-ui: art: %s %dx%d -> %dx%d for %s\n", r.source, r.src_w, r.src_h, r.w, r.h, uri);
        else      fprintf(stderr, "simpleton-ui: art: none for %s\n", uri);
        free(uri);

        pthread_mutex_lock(&mu);
        if(gen == req_gen && !req_uri) {          /* still the latest request */
            if(res_ready) free(res.pixels);       /* unread previous result */
            res = r;
            res_found = found;
            res_gen = gen;
            res_ready = true;
        }
        else free(r.pixels);                      /* stale: a newer request exists */
        pthread_mutex_unlock(&mu);
    }
    return NULL;
}

/* --------------------------------------------------------------- public */

bool art_init(int box)
{
    box_size = box;
    if(pthread_create(&worker, NULL, worker_main, NULL) != 0) {
        fprintf(stderr, "simpleton-ui: art: cannot start worker: %s\n", strerror(errno));
        return false;
    }
    pthread_detach(worker);
    return true;
}

void art_request(const char * track_uri)
{
    pthread_mutex_lock(&mu);
    free(req_uri);
    req_uri = strdup(track_uri);
    req_gen++;
    if(res_ready) { free(res.pixels); res_ready = false; }   /* obsolete */
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
}

bool art_poll(bool * found, art_result_t * out)
{
    bool got = false;
    pthread_mutex_lock(&mu);
    if(res_ready) {
        *out = res;
        *found = res_found;
        res_ready = false;
        memset(&res, 0, sizeof(res));
        got = true;
    }
    pthread_mutex_unlock(&mu);
    return got;
}
