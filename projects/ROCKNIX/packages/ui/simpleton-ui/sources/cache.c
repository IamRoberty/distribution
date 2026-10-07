/*
 * SimpletonOS - the library cache builder (implementation). See cache.h.
 */
#include "cache.h"
#include "art.h"
#include "mpdc.h"

#include <dirent.h>
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <utime.h>

#include <jpeglib.h>

#define MUSIC_DIR_DEFAULT "/storage/music"
#define THUMB_QUALITY     85
#define MIN_FREE_ON_CARD  (32u << 20)     /* keep this much free on the card, plus the file */

static const char * cache_root(void)
{
    const char * e = getenv("SIMPLETON_CACHE_ROOT");
    return e && e[0] ? e : CACHE_ROOT_DEFAULT;
}

static const char * music_dir(void)
{
    const char * e = getenv("SIMPLETON_MUSIC_DIR");
    return e && e[0] ? e : MUSIC_DIR_DEFAULT;
}

static const char * setting_file(void)
{
    const char * e = getenv("SIMPLETON_CACHE_SETTING");
    return e && e[0] ? e : CACHE_SETTING;
}

void cache_index_path(const char * card, char * out, size_t len)
{
    snprintf(out, len, "%s/%s/library.idx", cache_root(), card);
}

void cache_album_key(const lib_album_t * a, char * out, size_t len)
{
    uint64_t h = 1469598103934665603ull;
    const char * parts[3] = { a->folder, a->title, a->artist };
    for(int i = 0; i < 3; i++) {
        for(const char * s = parts[i]; *s; s++) { h ^= (uint8_t)*s; h *= 1099511628211ull; }
        h ^= 0x1f; h *= 1099511628211ull;
    }
    snprintf(out, len, "%016llx", (unsigned long long)h);
}

void cache_thumb_path(const char * card, const lib_album_t * album, int size_index, char * out, size_t len)
{
    char key[24];
    cache_album_key(album, key, sizeof(key));
    snprintf(out, len, "%s/%s/thumbs/%s_%d.jpg", cache_root(), card, key, lib_thumb_px[size_index]);
}

/* ---------------------------------------------------------------- files */

static bool mkdirs(const char * path)
{
    char tmp[1200];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for(char * p = tmp + 1; *p; p++) {
        if(*p != '/') continue;
        *p = 0;
        if(mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

static bool file_exists(const char * path)
{
    struct stat sb;
    return stat(path, &sb) == 0 && S_ISREG(sb.st_mode);
}

static bool dir_readable(const char * path)
{
    DIR * d = opendir(path);
    if(!d) return false;
    closedir(d);
    return true;
}

/* Copy `src` to `dst` through dst.tmp + rename, keeping the date. */
static bool copy_file(const char * src, const char * dst)
{
    FILE * in = fopen(src, "rb");
    if(!in) return false;
    char tmp[1300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
    FILE * out = fopen(tmp, "wb");
    if(!out) { fclose(in); return false; }
    char buf[65536];
    size_t n;
    bool ok = true;
    while((n = fread(buf, 1, sizeof(buf), in)) > 0) if(fwrite(buf, 1, n, out) != n) { ok = false; break; }
    if(ferror(in)) ok = false;
    fclose(in);
    if(fclose(out) != 0) ok = false;
    if(ok) {
        struct stat sb;
        if(stat(src, &sb) == 0) { struct utimbuf ut = { sb.st_atime, sb.st_mtime }; utime(tmp, &ut); }
        ok = rename(tmp, dst) == 0;
    }
    if(!ok) remove(tmp);
    return ok;
}

/* True when `dst` is missing, or `src` is newer or a different size. */
static bool needs_copy(const char * src, const char * dst)
{
    struct stat a, b;
    if(stat(src, &a) != 0) return false;
    if(stat(dst, &b) != 0) return true;
    return a.st_size != b.st_size || a.st_mtime > b.st_mtime + 2;     /* exFAT keeps even seconds */
}

/* ----------------------------------------------------------------- JPEG */

struct jerr { struct jpeg_error_mgr pub; jmp_buf jb; };
static void jerr_exit(j_common_ptr c) { longjmp(((struct jerr *)c->err)->jb, 1); }
static void jerr_msg(j_common_ptr c) { (void)c; }

/* Write an XRGB8888 picture as a JPEG, through path.tmp + rename. */
static bool write_jpeg(const char * path, const uint8_t * px, int w, int h)
{
    char tmp[1300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE * f = fopen(tmp, "wb");
    if(!f) return false;

    struct jpeg_compress_struct c;
    struct jerr err;
    c.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = jerr_exit;
    err.pub.output_message = jerr_msg;
    if(setjmp(err.jb)) { jpeg_destroy_compress(&c); fclose(f); remove(tmp); return false; }

    jpeg_create_compress(&c);
    jpeg_stdio_dest(&c, f);
    c.image_width = (JDIMENSION)w;
    c.image_height = (JDIMENSION)h;
    c.input_components = 4;
    c.in_color_space = JCS_EXT_BGRX;        /* XRGB8888 in memory is B,G,R,X */
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, THUMB_QUALITY, TRUE);
    jpeg_start_compress(&c, TRUE);
    while(c.next_scanline < c.image_height) {
        JSAMPROW row = (JSAMPROW)(px + (size_t)c.next_scanline * w * 4);
        jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    bool ok = fclose(f) == 0 && rename(tmp, path) == 0;
    if(!ok) remove(tmp);
    return ok;
}

/* Area-average shrink of XRGB8888 (the picture is already <= 2x the target
 * in each step, so this is as good as it needs to be). */
static uint8_t * shrink(const uint8_t * src, int sw, int sh, int dw, int dh)
{
    uint32_t * dst = malloc((size_t)dw * dh * 4);
    if(!dst) return NULL;
    for(int y = 0; y < dh; y++) {
        int y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
        if(y1 <= y0) y1 = y0 + 1;
        for(int x = 0; x < dw; x++) {
            int x0 = x * sw / dw, x1 = (x + 1) * sw / dw;
            if(x1 <= x0) x1 = x0 + 1;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for(int yy = y0; yy < y1; yy++) {
                const uint8_t * p = src + ((size_t)yy * sw + x0) * 4;
                for(int xx = x0; xx < x1; xx++, p += 4) { b += p[0]; g += p[1]; r += p[2]; n++; }
            }
            dst[(size_t)y * dw + x] = 0xFF000000u | ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
    }
    return (uint8_t *)dst;
}

uint32_t cache_dominant_colour(const uint8_t * px, int w, int h)
{
    /* fullest bin of a 4-bit-per-channel histogram, vivid pixels counting a
     * little extra, averaged within the bin - the now-playing side colour's
     * recipe without the muting */
    static uint32_t weight[4096], samples[4096], sum[4096][3];
    memset(weight, 0, sizeof(weight));
    memset(samples, 0, sizeof(samples));
    memset(sum, 0, sizeof(sum));
    int step = (w > h ? w : h) / 96;
    if(step < 1) step = 1;
    for(int y = 0; y < h; y += step) {
        const uint8_t * row = px + (size_t)y * w * 4;
        for(int x = 0; x < w; x += step) {
            int b = row[x * 4], g = row[x * 4 + 1], r = row[x * 4 + 2];
            int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
            int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
            int bin = ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
            weight[bin] += 2 + (mx ? 4 * (mx - mn) / mx : 0);
            samples[bin]++;
            sum[bin][0] += (uint32_t)r; sum[bin][1] += (uint32_t)g; sum[bin][2] += (uint32_t)b;
        }
    }
    int best = 0;
    for(int i = 1; i < 4096; i++) if(weight[i] > weight[best]) best = i;
    uint32_t n = samples[best];
    if(!n) return 0;
    return ((sum[best][0] / n) << 16) | ((sum[best][1] / n) << 8) | (sum[best][2] / n);
}

/* ------------------------------------------------------------ building */

typedef struct { bool on; char dir[1200]; } card_copy_t;

static bool all_thumbs_present(const char * card, const lib_album_t * a)
{
    char p[1300];
    for(int i = 0; i < LIB_THUMB_COUNT; i++) {
        cache_thumb_path(card, a, i, p, sizeof(p));
        if(!file_exists(p)) return false;
    }
    return true;
}

/* Fetch, decode and write every size for one album. Returns false when the
 * card went away (caller aborts); has_art false is a valid outcome. */
static bool make_thumbs(const char * card, const char * mount, lib_album_t * a, bool verbose)
{
    size_t len = 0;
    char source[64];
    uint8_t * data = art_fetch_cover(a->first_uri, &len, source, sizeof(source));
    if(!data) {
        if(!dir_readable(mount)) return false;              /* the card is gone, not the art */
        a->has_art = false;
        a->colour = 0;
        a->thumb_sizes = 0;
        if(verbose) fprintf(stderr, "simpleton-cache:   no art: %s / %s\n", a->artist, a->title);
        return true;
    }
    int big = lib_thumb_px[LIB_THUMB_COUNT - 1];
    int w = 0, h = 0;
    uint8_t * px = art_decode_fit(data, len, big, &w, &h);
    free(data);
    if(!px) {
        fprintf(stderr, "simpleton-cache:   could not decode %s (%zu bytes) for %s / %s\n", source, len, a->artist, a->title);
        a->has_art = false;
        a->colour = 0;
        a->thumb_sizes = 0;
        return true;
    }
    a->has_art = true;
    a->thumb_sizes = 0;
    /* largest first, each next size shrunk from the previous */
    uint8_t * cur = px;
    int cw = w, ch = h;
    for(int i = LIB_THUMB_COUNT - 1; i >= 0; i--) {
        int box = lib_thumb_px[i];
        int dw, dh;
        if(cw > ch) { dw = box; dh = (int)((long)ch * box / cw); } else { dh = box; dw = (int)((long)cw * box / ch); }
        if(dw < 1) dw = 1;
        if(dh < 1) dh = 1;
        uint8_t * out = (dw == cw && dh == ch) ? cur : shrink(cur, cw, ch, dw, dh);
        if(!out) break;
        char p[1300];
        cache_thumb_path(card, a, i, p, sizeof(p));
        if(write_jpeg(p, out, dw, dh)) a->thumb_sizes |= (uint8_t)(1u << i);
        if(i == 0) a->colour = cache_dominant_colour(out, dw, dh);
        if(out != cur) { if(cur != px) free(cur); cur = out; cw = dw; ch = dh; }
    }
    if(cur != px) free(cur);
    free(px);
    if(verbose) fprintf(stderr, "simpleton-cache:   %s %dx%d, colour %06x: %s / %s\n", source, w, h, a->colour, a->artist, a->title);
    return true;
}

/* Remove thumbnails that belong to no album in `lib`. */
static void prune_thumbs(const char * card, const library_t * lib)
{
    char dir[1300];
    snprintf(dir, sizeof(dir), "%s/%s/thumbs", cache_root(), card);
    DIR * d = opendir(dir);
    if(!d) return;
    /* keys in use */
    char (*keys)[17] = malloc((size_t)(lib->nalbums ? lib->nalbums : 1) * 17);
    if(!keys) { closedir(d); return; }
    for(int i = 0; i < lib->nalbums; i++) cache_album_key(&lib->albums[i], keys[i], 17);
    struct dirent * e;
    int removed = 0;
    while((e = readdir(d))) {
        if(strlen(e->d_name) < 18 || e->d_name[16] != '_') { if(strstr(e->d_name, ".tmp")) { char p[1600]; snprintf(p, sizeof(p), "%s/%s", dir, e->d_name); remove(p); } continue; }
        bool used = false;
        for(int i = 0; i < lib->nalbums && !used; i++) used = strncmp(keys[i], e->d_name, 16) == 0;
        if(!used) { char p[1600]; snprintf(p, sizeof(p), "%s/%s", dir, e->d_name); if(remove(p) == 0) removed++; }
    }
    closedir(d);
    free(keys);
    if(removed) fprintf(stderr, "simpleton-cache: removed %d thumbnail%s of albums no longer on the card\n", removed, removed == 1 ? "" : "s");
}

/* Mirror the cache to the card (or from it, when `to_card` is false). */
static void mirror(const char * from, const char * to, bool to_card, int * copied)
{
    char src[1300], dst[1300];
    *copied = 0;
    if(!mkdirs(to)) return;
    snprintf(src, sizeof(src), "%s/thumbs", from);
    snprintf(dst, sizeof(dst), "%s/thumbs", to);
    mkdirs(dst);
    DIR * d = opendir(src);
    if(d) {
        struct dirent * e;
        while((e = readdir(d))) {
            if(e->d_name[0] == '.' || strstr(e->d_name, ".tmp")) continue;
            char s[1600], t[1600];
            snprintf(s, sizeof(s), "%s/%s", src, e->d_name);
            snprintf(t, sizeof(t), "%s/%s", dst, e->d_name);
            if(!needs_copy(s, t)) continue;
            if(to_card) {
                struct statvfs vf;
                struct stat sb;
                if(statvfs(to, &vf) == 0 && stat(s, &sb) == 0 &&
                   (unsigned long long)vf.f_bavail * vf.f_frsize < (unsigned long long)sb.st_size + MIN_FREE_ON_CARD) {
                    fprintf(stderr, "simpleton-cache: card nearly full, stopped copying to it\n");
                    break;
                }
            }
            if(copy_file(s, t)) (*copied)++;
        }
        closedir(d);
    }
    /* thumbnails the source no longer has (albums gone from the card) */
    if(to_card) {
        DIR * dd = opendir(dst);
        if(dd) {
            struct dirent * e;
            while((e = readdir(dd))) {
                if(e->d_name[0] == '.') continue;
                char s2[1600], t2[1600];
                snprintf(s2, sizeof(s2), "%s/%s", src, e->d_name);
                snprintf(t2, sizeof(t2), "%s/%s", dst, e->d_name);
                if(!file_exists(s2)) remove(t2);
            }
            closedir(dd);
        }
    }
    /* the index last, so a reader never sees an index ahead of its thumbnails */
    snprintf(src, sizeof(src), "%s/library.idx", from);
    snprintf(dst, sizeof(dst), "%s/library.idx", to);
    if(needs_copy(src, dst) && copy_file(src, dst)) (*copied)++;
}

bool cache_build(const char * card, bool verbose)
{
    char sysdir[1200], carddir[1400], mount[1300], idx[1300], link[1300];
    snprintf(sysdir, sizeof(sysdir), "%s/%s", cache_root(), card);
    snprintf(link, sizeof(link), "%s/%s", music_dir(), card);
    ssize_t n = readlink(link, mount, sizeof(mount) - 1);
    if(n < 0) {
        /* not a symlink: a plain folder under the music dir is fine too */
        if(!dir_readable(link)) { fprintf(stderr, "simpleton-cache: %s: no such card (%s)\n", card, link); return false; }
        snprintf(mount, sizeof(mount), "%.1299s", link);
    }
    else mount[n] = 0;
    if(!dir_readable(mount)) { fprintf(stderr, "simpleton-cache: %s: %s is not readable\n", card, mount); return false; }
    snprintf(carddir, sizeof(carddir), "%s/%s", mount, CACHE_ON_CARD_DIR);
    cache_index_path(card, idx, sizeof(idx));

    char thumbs[1300];
    snprintf(thumbs, sizeof(thumbs), "%s/thumbs", sysdir);
    if(!mkdirs(thumbs)) { fprintf(stderr, "simpleton-cache: cannot create %s\n", thumbs); return false; }

    /* 0. a cache on the card and none here: start from the card's */
    char cardidx[1500];
    snprintf(cardidx, sizeof(cardidx), "%s/library.idx", carddir);
    if(!file_exists(idx) && file_exists(cardidx)) {
        int copied;
        mirror(carddir, sysdir, false, &copied);
        fprintf(stderr, "simpleton-cache: %s: took the cache from the card (%d files)\n", card, copied);
    }

    /* 1. the library as MPD has it now */
    library_t lib;
    if(!library_build(&lib, card)) { fprintf(stderr, "simpleton-cache: %s: MPD not reachable, nothing done\n", card); return false; }
    fprintf(stderr, "simpleton-cache: %s: %d tracks, %d albums, %d artists, %d folders\n", card, lib.ntracks, lib.nalbums, lib.nartists, lib.nfolders);

    /* 2. what the previous run knew */
    library_t old;
    bool have_old = library_load(&old, idx);

    int kept = 0, made = 0, noart = 0;
    for(int i = 0; i < lib.nalbums; i++) {
        lib_album_t * a = &lib.albums[i];
        const lib_album_t * o = NULL;
        if(have_old) {
            for(int k = 0; k < old.nalbums; k++) {
                const lib_album_t * c = &old.albums[k];
                if(c->fingerprint == a->fingerprint && strcmp(c->folder, a->folder) == 0 && strcmp(c->title, a->title) == 0 && strcmp(c->artist, a->artist) == 0) { o = c; break; }
            }
        }
        if(o && (!o->has_art || all_thumbs_present(card, a))) {
            a->has_art = o->has_art;
            a->colour = o->colour;
            a->thumb_sizes = o->thumb_sizes;
            kept++;
            continue;
        }
        if(!make_thumbs(card, mount, a, verbose)) {
            fprintf(stderr, "simpleton-cache: %s: the card went away, stopping (cache left as it was)\n", card);
            if(have_old) library_free(&old);
            library_free(&lib);
            return false;
        }
        if(a->has_art) made++; else noart++;
    }
    if(have_old) library_free(&old);

    /* 3. the index, then the leftovers */
    bool ok = library_save(&lib, idx);
    if(!ok) fprintf(stderr, "simpleton-cache: %s: could not write %s\n", card, idx);
    prune_thumbs(card, &lib);
    fprintf(stderr, "simpleton-cache: %s: %d albums unchanged, %d thumbnailed, %d without art; index %s\n",
            card, kept, made, noart, ok ? "written" : "NOT written");

    /* 4. the copy on the card */
    char setting[16] = "";
    FILE * f = fopen(setting_file(), "r");
    if(f) { if(fgets(setting, sizeof(setting), f)) setting[strcspn(setting, " \r\n")] = 0; fclose(f); }
    if(ok && strcmp(setting, "off") != 0) {
        int copied;
        mirror(sysdir, carddir, true, &copied);
        if(copied) fprintf(stderr, "simpleton-cache: %s: %d file%s copied to the card\n", card, copied, copied == 1 ? "" : "s");
    }
    library_free(&lib);
    return ok;
}
