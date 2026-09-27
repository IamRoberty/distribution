/*
 * SimpletonOS boot splash.
 *
 * Replaces rocknix-splash's main.c (SimpletonOS package override, 27 Sep
 * 2026): the ROCKNIX build copies this file over the upstream one via the
 * package's sources/ folder. Upstream drew the ROCKNIX wordmark from vector
 * paths compiled into the binary; this draws the SIMPLETON wordmark from an
 * 8-bit coverage mask (simpleton_logo.h), tinted and scaled to 60% of the
 * screen width, centred on black.
 *
 * Deliberately self-contained: it talks to /dev/fb0 directly and uses none
 * of upstream's helpers, so an upstream change to fbsplash.c's API can't
 * break it. The other upstream files still compile; they're just unused.
 *
 * Not handled: display rotation from the device tree (upstream's
 * dt_rotation.c). The RGB20SX panel isn't rotated; revisit for a device
 * that is.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "simpleton_logo.h"

#define LOGO_COLOR_R 0xE8          /* UI_COLOR_FG from simpleton-ui theme.h */
#define LOGO_COLOR_G 0xE8
#define LOGO_COLOR_B 0xE8
#define LOGO_WIDTH_FRACTION 0.6f   /* same proportion upstream used */

static uint8_t mask_at(int x, int y)
{
    if(x < 0 || y < 0 || x >= LOGO_W || y >= LOGO_H) return 0;
    return logo_mask[y * LOGO_W + x];
}

/* Coverage of destination pixel (dx,dy) for a logo scaled to dw x dh.
 * Shrinking: average the source box it covers. Enlarging: bilinear. */
static uint8_t sample(int dx, int dy, int dw, int dh)
{
    if(dw <= LOGO_W) {
        int x0 = dx * LOGO_W / dw, x1 = (dx + 1) * LOGO_W / dw;
        int y0 = dy * LOGO_H / dh, y1 = (dy + 1) * LOGO_H / dh;
        if(x1 <= x0) x1 = x0 + 1;
        if(y1 <= y0) y1 = y0 + 1;
        unsigned sum = 0, n = 0;
        for(int y = y0; y < y1; y++)
            for(int x = x0; x < x1; x++) { sum += mask_at(x, y); n++; }
        return (uint8_t)(sum / n);
    }
    float fx = (dx + 0.5f) * LOGO_W / dw - 0.5f;
    float fy = (dy + 0.5f) * LOGO_H / dh - 0.5f;
    int x0 = (int)fx, y0 = (int)fy;
    if(fx < 0) { x0 = 0; fx = 0; }
    if(fy < 0) { y0 = 0; fy = 0; }
    float tx = fx - x0, ty = fy - y0;
    float top = mask_at(x0, y0) + (mask_at(x0 + 1, y0) - mask_at(x0, y0)) * tx;
    float bot = mask_at(x0, y0 + 1) + (mask_at(x0 + 1, y0 + 1) - mask_at(x0, y0 + 1)) * tx;
    float v = top + (bot - top) * ty;
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f);
}

static void put(uint8_t * buf, const struct fb_var_screeninfo * vi, const struct fb_fix_screeninfo * fi,
                int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    size_t bpp = vi->bits_per_pixel / 8;
    size_t off = (size_t)(y + vi->yoffset) * fi->line_length + (size_t)(x + vi->xoffset) * bpp;
    if(vi->bits_per_pixel == 32) {
        uint32_t px = ((uint32_t)r >> (8 - vi->red.length)) << vi->red.offset
                    | ((uint32_t)g >> (8 - vi->green.length)) << vi->green.offset
                    | ((uint32_t)b >> (8 - vi->blue.length)) << vi->blue.offset;
        if(vi->transp.length) px |= ((1u << vi->transp.length) - 1) << vi->transp.offset;
        memcpy(buf + off, &px, 4);
    }
    else if(vi->bits_per_pixel == 16) {
        uint16_t px = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        memcpy(buf + off, &px, 2);
    }
}

int main(void)
{
    const char * dev = "/dev/fb0";
    int fd = open(dev, O_RDWR);
    if(fd < 0) { fprintf(stderr, "simpleton-splash: cannot open %s: %s\n", dev, strerror(errno)); return 1; }

    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    if(ioctl(fd, FBIOGET_VSCREENINFO, &vi) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fi) < 0) {
        fprintf(stderr, "simpleton-splash: framebuffer info: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    if(vi.bits_per_pixel != 32 && vi.bits_per_pixel != 16) {
        fprintf(stderr, "simpleton-splash: unsupported depth %u bpp\n", vi.bits_per_pixel);
        close(fd);
        return 1;
    }

    size_t size = (size_t)fi.line_length * vi.yres_virtual;
    uint8_t * buf = calloc(1, size);          /* all zero = black */
    if(!buf) { close(fd); return 1; }
    for(uint32_t y = 0; y < vi.yres; y++)     /* explicit black: sets alpha where the format has it */
        for(uint32_t x = 0; x < vi.xres; x++) put(buf, &vi, &fi, (int)x, (int)y, 0, 0, 0);

    int dw = (int)(vi.xres * LOGO_WIDTH_FRACTION);
    int dh = (int)((long)dw * LOGO_H / LOGO_W);
    if(dh > (int)(vi.yres * LOGO_WIDTH_FRACTION)) {
        dh = (int)(vi.yres * LOGO_WIDTH_FRACTION);
        dw = (int)((long)dh * LOGO_W / LOGO_H);
    }
    int ox = ((int)vi.xres - dw) / 2, oy = ((int)vi.yres - dh) / 2;

    for(int y = 0; y < dh; y++) {
        for(int x = 0; x < dw; x++) {
            unsigned a = sample(x, y, dw, dh);
            if(!a) continue;
            put(buf, &vi, &fi, ox + x, oy + y,
                (uint8_t)(LOGO_COLOR_R * a / 255), (uint8_t)(LOGO_COLOR_G * a / 255), (uint8_t)(LOGO_COLOR_B * a / 255));
        }
    }

    lseek(fd, 0, SEEK_SET);
    size_t done = 0;
    while(done < size) {
        ssize_t n = write(fd, buf + done, size - done);
        if(n <= 0) break;
        done += (size_t)n;
    }
    free(buf);
    close(fd);
    return 0;
}
