/*
 * SimpletonOS UI - is this track DST? See dst.h.
 */
#include "dst.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MUSIC_DIR_DEFAULT "/storage/music"
static const char * music_dir(void)
{
    static const char * dir;
    if(!dir) { const char * e = getenv("SIMPLETON_MUSIC_DIR"); dir = e && e[0] ? e : MUSIC_DIR_DEFAULT; }
    return dir;
}

static uint32_t be32(const uint8_t * p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t be64(const uint8_t * p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

static bool read_at(FILE * f, uint64_t off, uint8_t * buf, size_t n)
{
    return fseeko(f, (off_t)off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}

/* An SACD ISO: sectors of 2048 bytes, or 2064 with a 12-byte header (the
 * raw form); the master TOC at sector 510 names the area TOCs; the stereo
 * area's ("TWOCHTOC") byte 21, low nibble, is the frame format: 0 = DST. */
static bool iso_is_dst(FILE * f)
{
    uint8_t m[2048];
    uint64_t sector = 2048, off = 0;
    if(!read_at(f, 510ull * 2048, m, 8) || memcmp(m, "SACDMTOC", 8) != 0) {
        sector = 2064; off = 12;
        if(!read_at(f, 510ull * 2064 + 12, m, 8) || memcmp(m, "SACDMTOC", 8) != 0) return false;
    }
    if(!read_at(f, 510ull * sector + off, m, sizeof(m))) return false;
    uint32_t starts[2] = { be32(m + 64), be32(m + 72) };            /* area_1_toc_1_start, area_2_toc_1_start */
    int dst = -1, any = -1;
    for(int i = 0; i < 2; i++) {
        if(!starts[i]) continue;
        uint8_t a[64];
        if(!read_at(f, (uint64_t)starts[i] * sector + off, a, sizeof(a))) continue;
        bool two = memcmp(a, "TWOCHTOC", 8) == 0, mul = memcmp(a, "MULCHTOC", 8) == 0;
        if(!two && !mul) continue;
        int v = (a[21] & 0x0F) == 0;
        if(two) dst = v; else if(any < 0) any = v;
    }
    if(dst < 0) dst = any;                                           /* no stereo area: the multichannel one (MPD plays what there is) */
    return dst > 0;
}

/* A DSDIFF file: FRM8 / size / "DSD ", then chunks of id + 64-bit size;
 * PROP holds "SND " and sub-chunks, among them CMPR with the compression
 * type: "DST " or "DSD ". The header sits at the front: 64 KB is plenty. */
static bool dff_is_dst(FILE * f)
{
    static uint8_t b[65536];
    size_t n = fread(b, 1, sizeof(b), f);
    if(n < 16 || memcmp(b, "FRM8", 4) != 0 || memcmp(b + 12, "DSD ", 4) != 0) return false;
    size_t p = 16;
    while(p + 12 <= n) {
        uint64_t sz = be64(b + p + 4);
        if(memcmp(b + p, "PROP", 4) == 0) {
            size_t q = p + 12 + 4, end = p + 12 + (size_t)(sz < n ? sz : n);   /* past "SND " */
            while(q + 12 <= end && q + 12 <= n) {
                uint64_t s2 = be64(b + q + 4);
                if(memcmp(b + q, "CMPR", 4) == 0) return q + 16 <= n && memcmp(b + q + 12, "DST ", 4) == 0;
                q += 12 + (size_t)s2 + (s2 & 1);
            }
            return false;
        }
        p += 12 + (size_t)sz + (sz & 1);
    }
    return false;
}

bool dst_track(const char * uri)
{
    static char last_path[2048]; static bool last_dst;
    if(!uri || !uri[0]) return false;
    /* the container file: the part up to ".iso/" or ".dff/", else the file itself */
    char path[2048];
    snprintf(path, sizeof(path), "%s/%s", music_dir(), uri);
    for(char * p = path; *p; p++)
        if(*p == '.' && (strncasecmp(p, ".iso/", 5) == 0 || strncasecmp(p, ".dff/", 5) == 0)) { p[4] = 0; break; }
    size_t len = strlen(path);
    bool iso = len > 4 && strcasecmp(path + len - 4, ".iso") == 0;
    bool dff = len > 4 && strcasecmp(path + len - 4, ".dff") == 0;
    if(!iso && !dff) return false;
    if(strcmp(path, last_path) == 0) return last_dst;
    FILE * f = fopen(path, "rb");
    bool dst = false;
    if(f) { dst = iso ? iso_is_dst(f) : dff_is_dst(f); fclose(f); }
    snprintf(last_path, sizeof(last_path), "%s", path);
    last_dst = dst;
    if(dst) fprintf(stderr, "simpleton-ui: dst: %s is DST\n", path);
    return dst;
}
