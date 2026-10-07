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
#include "placeholder.h"

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
 * socket, not TCP, so this is fixed here and matches mpd.conf. Cards are
 * symlinks below it (card-<serial>, 0.12), so a track's URI maps straight
 * to a path. $SIMPLETON_MUSIC_DIR overrides it for off-device tests. */
#define MUSIC_DIR_DEFAULT "/storage/music"
static const char * music_dir(void)
{
    static const char * dir;
    if(!dir) { const char * e = getenv("SIMPLETON_MUSIC_DIR"); dir = e && e[0] ? e : MUSIC_DIR_DEFAULT; }
    return dir;
}
#define MUSIC_DIR music_dir()

#define MAX_IMAGE_BYTES (64u << 20)

/* ---------------------------------------------------------------- state */

static int box_size = 720;

static pthread_t       worker;
static pthread_mutex_t mu   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv   = PTHREAD_COND_INITIALIZER;

/* request slot (latest wins) */
static char *   req_uri;
static char     req_artist[256], req_album[256], req_title[256];
static unsigned req_gen;
static int      req_page = -1;           /* art paging request, -1 = none */

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

/* Case-insensitive search for a container marker in a URI - ".iso/",
 * ".cue/", and since 0.12 ".dff/" and ".dat/" (the SACD patch presents a
 * DFF as a container of its tracks: "01 - Black Cow.dff/2C_AUDIO__TRACK001.dff";
 * without this the album folder was taken to be the .dff file itself and
 * DFF albums never found their folder cover). Returns the '.' or NULL. */
static const char * find_container(const char * uri)
{
    for(const char * p = uri; *p; p++) {
        if(*p != '.') continue;
        if(strncasecmp(p, ".iso/", 5) == 0 || strncasecmp(p, ".cue/", 5) == 0 ||
           strncasecmp(p, ".dff/", 5) == 0 || strncasecmp(p, ".dat/", 5) == 0) return p;
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

static const char * const art_subdirs[] = { "artwork", "scans", "covers", "cover", "art", "images", "scan", NULL };

/* Windows Media Player's hidden thumbnails and macOS "._" files aren't art. */
static bool skip_image(const char * name)
{
    return name[0] == '.' || strncasecmp(name, "albumartsmall", 13) == 0 || strncasecmp(name, "albumart_{", 10) == 0;
}

static bool is_art_subdir(const char * name)
{
    for(int s = 0; art_subdirs[s]; s++) if(strcasecmp(name, art_subdirs[s]) == 0) return true;
    return false;
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
    struct dirent ** ents;
    int n = scandir(dir, &ents, NULL, alpha_cmp);
    if(n < 0) return NULL;
    char * best = NULL;
    for(int i = 0; i < n && !best; i++) {
        const char * name = ents[i]->d_name;
        if(!is_art_subdir(name)) continue;

        char sub[1600];
        snprintf(sub, sizeof(sub), "%s/%s", dir, name);
        struct dirent ** files;
        int m = scandir(sub, &files, NULL, alpha_cmp);
        if(m < 0) continue;
        char * first = NULL, * front = NULL, * cover = NULL;
        for(int j = 0; j < m; j++) {
            const char * fn = files[j]->d_name;
            if(!skip_image(fn) && has_ext(fn, image_exts)) {
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

/* ---------------------------------------------------- album image list */

#define MAX_PAGES 64

static void add_images_in(const char * dir, char ** list, int * n)
{
    struct dirent ** ents;
    int m = scandir(dir, &ents, NULL, alpha_cmp);
    if(m < 0) return;
    for(int i = 0; i < m; i++) {
        const char * name = ents[i]->d_name;
        char * p;
        if(*n < MAX_PAGES && !skip_image(name) && has_ext(name, image_exts) && asprintf(&p, "%s/%s", dir, name) >= 0)
            list[(*n)++] = p;
        free(ents[i]);
    }
    free(ents);
}

/* Every image in the album folder, then in each artwork subfolder (one
 * level deep), alphabetical within each. */
static int collect_images(const char * dir, char ** list)
{
    int n = 0;
    add_images_in(dir, list, &n);
    struct dirent ** ents;
    int m = scandir(dir, &ents, NULL, alpha_cmp);
    if(m < 0) return n;
    for(int i = 0; i < m; i++) {
        if(is_art_subdir(ents[i]->d_name)) {
            char sub[1600];
            snprintf(sub, sizeof(sub), "%s/%s", dir, ents[i]->d_name);
            add_images_in(sub, list, &n);
        }
        free(ents[i]);
    }
    free(ents);
    return n;
}

/* True when the file at `path` holds exactly these bytes: an embedded cover
 * that was also saved as folder.jpg shouldn't be paged to twice. */
static bool same_bytes(const char * path, const uint8_t * data, size_t len)
{
    struct stat sb;
    if(stat(path, &sb) != 0 || (size_t)sb.st_size != len) return false;
    size_t flen;
    uint8_t * f = read_file(path, &flen);
    bool same = f && flen == len && memcmp(f, data, len) == 0;
    free(f);
    return same;
}

/* ------------------------------------------------------------- decode */

typedef struct { uint8_t * rgb; int w, h; int src_w, src_h; int dst_w, dst_h; } rgb_image_t;

/* Wider than this and a page is shown full height and panned instead of
 * shrunk to fit: a 2:1 gatefold fitted to a square is a thin strip. */
#define WIDE_ASPECT 1.2
#define MAX_ASPECT  4.0

/* On-screen size for a source image. Fitted inside the box, or for a wide
 * page (`pan` true) the box height with the width following, up to 4:1. */
static void target_size(int sw, int sh, bool pan, int * dw, int * dh)
{
    double aspect = (double)sw / sh, scale;
    if(pan && aspect >= WIDE_ASPECT)
        scale = aspect <= MAX_ASPECT ? (double)box_size / sh : box_size * MAX_ASPECT / sw;
    else
        scale = (double)box_size / (sw > sh ? sw : sh);
    *dw = (int)lround(sw * scale);
    *dh = (int)lround(sh * scale);
    if(*dw < 1) *dw = 1;
    if(*dh < 1) *dh = 1;
}

struct jerr { struct jpeg_error_mgr pub; jmp_buf jb; };
static void jerr_exit(j_common_ptr c) { longjmp(((struct jerr *)c->err)->jb, 1); }
static void jerr_msg(j_common_ptr c) { (void)c; }   /* quiet */

static bool decode_jpeg(const uint8_t * data, size_t len, bool pan, rgb_image_t * out)
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

    /* DCT scaling: shrink as far as possible while staying >= the final
     * size, so the resampler below only ever works on <= 2x. */
    target_size(out->src_w, out->src_h, pan, &out->dst_w, &out->dst_h);
    unsigned denom = 1;
    while(denom < 8 && out->src_w / (int)(denom * 2) >= out->dst_w && out->src_h / (int)(denom * 2) >= out->dst_h) denom *= 2;
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

static bool decode_png(const uint8_t * data, size_t len, bool pan, rgb_image_t * out)
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
    target_size(out->w, out->h, pan, &out->dst_w, &out->dst_h);
    return true;
}

static bool decode_any(const uint8_t * data, size_t len, bool pan, rgb_image_t * out)
{
    memset(out, 0, sizeof(*out));
    if(len > 3 && data[0] == 0xFF && data[1] == 0xD8) return decode_jpeg(data, len, pan, out);
    if(len > 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) return decode_png(data, len, pan, out);
    return false;
}

/* Scale `src` to its target size (see target_size), producing XRGB8888.
 * Box filter when shrinking (no aliasing on a 2x reduction), bilinear when
 * enlarging. */
static uint8_t * resample_fit(const rgb_image_t * src, int * out_w, int * out_h)
{
    int dw = src->dst_w, dh = src->dst_h;
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
    if(cont) return NULL;                /* ISO / DFF container: nothing embedded MPD can read */
    if(mpd_readpicture(uri, &data, len)) { snprintf(source, slen, "embedded"); return data; }
    return NULL;
}

static uint8_t * fetch_folder(const char * dir, size_t * len, char * source, size_t slen, char ** path_out)
{
    char * path = pick_named(dir);
    if(!path) path = pick_in_subfolder(dir);
    if(!path) return NULL;
    uint8_t * data = read_file(path, len);
    if(!data) { free(path); return NULL; }
    const char * base = strrchr(path, '/');
    snprintf(source, slen, "%s", base ? base + 1 : path);
    *path_out = path;
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

/* Worker-only state: the current album's pages. pages[0] is the cover's
 * path, or NULL when the cover is embedded or the placeholder; `cover` keeps
 * page 0 decoded so paging back to it is instant. */
static char *        pages[MAX_PAGES];
static int           page_count, page_cur;
static char          page_dir[1200];
static art_result_t  cover;

static bool dup_result(const art_result_t * src, art_result_t * dst)
{
    *dst = *src;
    dst->pixels = NULL;
    if(!src->pixels) return false;
    size_t n = (size_t)src->w * (size_t)src->h * 4;
    dst->pixels = malloc(n);
    if(!dst->pixels) return false;
    memcpy(dst->pixels, src->pixels, n);
    return true;
}

static bool decode_file(const char * path, art_result_t * r)
{
    size_t len;
    uint8_t * data = read_file(path, &len);
    const char * rel = path + strlen(page_dir);
    if(*rel == '/') rel++;
    snprintf(r->source, sizeof(r->source), "%s", rel);
    if(!data) return false;
    rgb_image_t img;
    bool ok = decode_any(data, len, true, &img);
    free(data);
    if(!ok) return false;
    r->pixels = resample_fit(&img, &r->w, &r->h);
    r->src_w = img.src_w;
    r->src_h = img.src_h;
    free(img.rgb);
    return r->pixels != NULL;
}

/* Page i of the current album; falls back to the cover if the file won't
 * decode, so the screen never goes blank mid-browse. */
static bool serve_page(int i, art_result_t * r)
{
    memset(r, 0, sizeof(*r));
    if(i > 0 && decode_file(pages[i], r)) return true;
    if(i > 0) fprintf(stderr, "simpleton-ui: art: could not decode %s, showing the cover\n", r->source);
    return dup_result(&cover, r);
}

static void clear_pages(void)
{
    for(int i = 0; i < page_count; i++) free(pages[i]);
    page_count = 0;
    page_cur = 0;
}

/* Track changed: find the cover, list the album's other images. */
static bool load_track(const char * uri, const char * artist, const char * album, const char * title,
                       art_result_t * r, uint64_t * cache_key, art_result_t * cache)
{
    char dir[1200];
    album_dir(uri, dir, sizeof(dir));

    /* remember the image on screen: within one album it stays up */
    char * keep = NULL;
    if(page_cur > 0 && strcmp(dir, page_dir) == 0) keep = strdup(pages[page_cur]);
    clear_pages();
    snprintf(page_dir, sizeof(page_dir), "%s", dir);

    char * list[MAX_PAGES];
    int n = collect_images(dir, list);

    memset(r, 0, sizeof(*r));
    size_t len = 0;
    char * cover_path = NULL;
    uint8_t * data = fetch_embedded(uri, &len, r->source, sizeof(r->source));
    bool embedded = data != NULL;
    if(!data) data = fetch_folder(dir, &len, r->source, sizeof(r->source), &cover_path);
    if(!data && n > 0) {
        /* images in the folder, none with a standard name: use the first */
        cover_path = strdup(list[0]);
        data = cover_path ? read_file(cover_path, &len) : NULL;
        if(data) snprintf(r->source, sizeof(r->source), "%s", strrchr(cover_path, '/') + 1);
    }

    pages[page_count++] = cover_path;
    for(int i = 0; i < n; i++) {
        bool dup = (cover_path && strcmp(list[i], cover_path) == 0) || (embedded && same_bytes(list[i], data, len));
        if(dup) free(list[i]);
        else pages[page_count++] = list[i];
    }

    bool found = false;
    if(data) {
        uint64_t key = fnv(data, len);
        if(cache->pixels && key == *cache_key) {
            char src[64];
            snprintf(src, sizeof(src), "%s", r->source);
            found = dup_result(cache, r);
            snprintf(r->source, sizeof(r->source), "%s", src);
        }
        else {
            rgb_image_t img;
            if(decode_any(data, len, false, &img)) {
                r->pixels = resample_fit(&img, &r->w, &r->h);
                r->src_w = img.src_w;
                r->src_h = img.src_h;
                free(img.rgb);
                if(r->pixels) {
                    found = true;
                    free(cache->pixels);
                    if(dup_result(r, cache)) *cache_key = key;
                }
            }
            else fprintf(stderr, "simpleton-ui: art: could not decode %s (%zu bytes) for %s\n", r->source, len, uri);
        }
        free(data);
    }
    if(!found) {
        /* no real art anywhere: draw the theme's placeholder for this track */
        r->pixels = placeholder_render(artist, album, title, &r->w, &r->h);
        if(r->pixels) { found = true; r->src_w = r->w; r->src_h = r->h; snprintf(r->source, sizeof(r->source), "placeholder"); }
    }
    if(found) fprintf(stderr, "simpleton-ui: art: %s %dx%d -> %dx%d for %s (%d image%s)\n",
                      r->source, r->src_w, r->src_h, r->w, r->h, uri, page_count, page_count == 1 ? "" : "s");
    else      fprintf(stderr, "simpleton-ui: art: none for %s\n", uri);

    free(cover.pixels);
    memset(&cover, 0, sizeof(cover));
    if(found) dup_result(r, &cover);

    if(keep) {
        for(int i = 1; i < page_count; i++) {
            if(strcmp(pages[i], keep) == 0) {
                free(r->pixels);
                page_cur = i;
                found = serve_page(i, r);
                break;
            }
        }
        free(keep);
    }
    r->index = page_cur;
    r->count = page_count;
    return found;
}

static void * worker_main(void * arg)
{
    (void)arg;
    /* cache of the last cover decode, keyed by the encoded bytes' hash */
    uint64_t cache_key = 0;
    art_result_t cache = { 0 };

    for(;;) {
        pthread_mutex_lock(&mu);
        while(!req_uri && req_page < 0) pthread_cond_wait(&cv, &mu);
        char * uri = req_uri;
        int page = req_page;
        unsigned gen = req_gen;
        char artist[256], album[256], title[256];
        snprintf(artist, sizeof(artist), "%s", req_artist);
        snprintf(album, sizeof(album), "%s", req_album);
        snprintf(title, sizeof(title), "%s", req_title);
        req_uri = NULL;
        req_page = -1;
        pthread_mutex_unlock(&mu);

        art_result_t r;
        bool found;
        if(uri) {
            found = load_track(uri, artist, album, title, &r, &cache_key, &cache);
            free(uri);
        }
        else {
            if(page_count <= 1) continue;
            page_cur = page % page_count;
            found = serve_page(page_cur, &r);
            r.index = page_cur;
            r.count = page_count;
            fprintf(stderr, "simpleton-ui: art: page %d/%d %s %dx%d -> %dx%d\n",
                    page_cur + 1, page_count, page_cur ? r.source : "cover", r.src_w, r.src_h, r.w, r.h);
        }

        pthread_mutex_lock(&mu);
        if(gen == req_gen && !req_uri && req_page < 0) {   /* still the latest request */
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
    placeholder_init(box);                  /* logs and returns false if assets are missing */
    if(pthread_create(&worker, NULL, worker_main, NULL) != 0) {
        fprintf(stderr, "simpleton-ui: art: cannot start worker: %s\n", strerror(errno));
        return false;
    }
    pthread_detach(worker);
    return true;
}

void art_request(const char * track_uri, const char * artist, const char * album, const char * title)
{
    pthread_mutex_lock(&mu);
    free(req_uri);
    req_uri = strdup(track_uri);
    snprintf(req_artist, sizeof(req_artist), "%s", artist ? artist : "");
    snprintf(req_album, sizeof(req_album), "%s", album ? album : "");
    snprintf(req_title, sizeof(req_title), "%s", title ? title : "");
    req_page = -1;
    req_gen++;
    if(res_ready) { free(res.pixels); res_ready = false; }   /* obsolete */
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
}

void art_request_page(int index)
{
    if(index < 0) index = 0;
    pthread_mutex_lock(&mu);
    if(!req_uri) {                        /* a pending track change wins */
        req_page = index;
        req_gen++;
        if(res_ready) { free(res.pixels); res_ready = false; }
        pthread_cond_signal(&cv);
    }
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

/* ------------------------------------------- synchronous, for the cache */

uint8_t * art_fetch_cover(const char * track_uri, size_t * len, char * source, size_t slen)
{
    char dir[1200];
    album_dir(track_uri, dir, sizeof(dir));
    *len = 0;
    source[0] = 0;
    uint8_t * data = fetch_embedded(track_uri, len, source, slen);
    if(data) return data;
    char * path = NULL;
    data = fetch_folder(dir, len, source, slen, &path);
    free(path);
    if(data) return data;
    char * list[MAX_PAGES];
    int n = collect_images(dir, list);
    if(n > 0) {
        data = read_file(list[0], len);
        if(data) snprintf(source, slen, "%s", strrchr(list[0], '/') + 1);
    }
    for(int i = 0; i < n; i++) free(list[i]);
    return data;
}

uint8_t * art_decode_fit(const uint8_t * data, size_t len, int box, int * w, int * h)
{
    int saved = box_size;
    box_size = box > 0 ? box : 1;
    rgb_image_t img;
    uint8_t * out = NULL;
    if(decode_any(data, len, false, &img)) {
        out = resample_fit(&img, w, h);
        free(img.rgb);
    }
    box_size = saved;
    return out;
}
