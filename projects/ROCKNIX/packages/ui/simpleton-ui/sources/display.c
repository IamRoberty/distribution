/*
 * SimpletonOS UI - display output (implementation). See display.h.
 *
 * The buffer/flip handling follows LVGL 9.5's own dumb-buffer DRM driver
 * (two full-screen buffers, LV_DISPLAY_RENDER_MODE_DIRECT, one atomic
 * page flip per refresh) so rendering behaves exactly as before; what's new
 * is which connector, which mode, and switching the other outputs off.
 */
#include "display.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <linux/netlink.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#define MAX_EXT_W        1920
#define MAX_EXT_H        1080
#define HOTPLUG_SETTLE_MS 800      /* HDMI flaps while the plug goes in */

typedef struct {
    uint32_t handle, pitch, fb;
    uint64_t size;
    uint8_t * map;
} fbuf_t;

static int drm_fd = -1;
static uint32_t conn_id, crtc_id, plane_id, mode_blob;
static drmModeModeInfo mode;
static fbuf_t bufs[2];
static fbuf_t * act_buf;
static drmModeAtomicReq * pending;
static bool modeset_done;
static bool switch_others_off = true;   /* false: light ours, touch nothing else (fallback) */
static drmEventContext evctx;

/* plane property ids, looked up once */
static uint32_t p_fb, p_crtc, p_sx, p_sy, p_sw, p_sh, p_cx, p_cy, p_cw, p_ch;

static pthread_mutex_t commit_lock = PTHREAD_MUTEX_INITIALIZER;   /* the UI's flips and the viz plane's commits (0.31) */
static uint32_t viz_plane;              /* the visualizer's plane, 0 = none */
static bool     viz_scales;             /* it may scale a smaller buffer to the screen */
static int      viz_zpos = -1;          /* the zpos to ask for, -1 = leave it alone */
static bool     viz_probed, viz_ok;
static bool     viz_takeover;           /* no spare plane: the visualizer borrows the UI's own (0.31c) */
static bool     viz_owns;               /* ...and holds it right now: the UI's flips wait */
static fbuf_t * ui_front;               /* the UI buffer last put on screen */
static int  uevent_fd = -1;
static uint32_t settle_at;         /* 0 = nothing pending */
static bool on_external;
static uint32_t next_port_check;
#define PORT_CHECK_MS 2000

/* ------------------------------------------------------------ helpers */

static uint32_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static uint32_t prop_id(uint32_t obj, uint32_t type, const char * name)
{
    drmModeObjectProperties * props = drmModeObjectGetProperties(drm_fd, obj, type);
    if(!props) return 0;
    uint32_t id = 0;
    for(uint32_t i = 0; i < props->count_props && !id; i++) {
        drmModePropertyRes * p = drmModeGetProperty(drm_fd, props->props[i]);
        if(p && strcmp(p->name, name) == 0) id = p->prop_id;
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return id;
}

static uint64_t prop_value(uint32_t obj, uint32_t type, const char * name)
{
    drmModeObjectProperties * props = drmModeObjectGetProperties(drm_fd, obj, type);
    if(!props) return 0;
    uint64_t v = 0;
    for(uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyRes * p = drmModeGetProperty(drm_fd, props->props[i]);
        bool hit = p && strcmp(p->name, name) == 0;
        if(hit) v = props->prop_values[i];
        drmModeFreeProperty(p);
        if(hit) break;
    }
    drmModeFreeObjectProperties(props);
    return v;
}

static void add(drmModeAtomicReq * req, uint32_t obj, uint32_t type, const char * name, uint64_t value)
{
    uint32_t id = prop_id(obj, type, name);
    if(id) drmModeAtomicAddProperty(req, obj, id, value);
}

static bool is_external(uint32_t type)
{
    return type == DRM_MODE_CONNECTOR_HDMIA || type == DRM_MODE_CONNECTOR_HDMIB ||
           type == DRM_MODE_CONNECTOR_DisplayPort || type == DRM_MODE_CONNECTOR_DVII ||
           type == DRM_MODE_CONNECTOR_DVID || type == DRM_MODE_CONNECTOR_DVIA;
}

static void conn_name(const drmModeConnector * c, char * out, size_t len)
{
    const char * n = drmModeGetConnectorTypeName(c->connector_type);
    snprintf(out, len, "%s-%u", n ? n : "unknown", c->connector_type_id);
}

/* Best mode for a TV: at most 1080 lines and 1920 wide, progressive; most
 * lines first, then closest to 60 Hz, then the TV's own preference. */
static int pick_external_mode(const drmModeConnector * c)
{
    int best = -1;
    long best_score = -1;
    for(int i = 0; i < c->count_modes; i++) {
        const drmModeModeInfo * m = &c->modes[i];
        if(m->flags & DRM_MODE_FLAG_INTERLACE) continue;
        if(m->hdisplay > MAX_EXT_W || m->vdisplay > MAX_EXT_H) continue;
        int hz_off = abs((int)m->vrefresh - 60);
        long score = (long)m->vdisplay * 100000 + (long)m->hdisplay * 10
                     + (hz_off <= 1 ? 5 : 0) + ((m->type & DRM_MODE_TYPE_PREFERRED) ? 2 : 0);
        if(score > best_score) { best_score = score; best = i; }
    }
    return best >= 0 ? best : 0;       /* nothing small enough: native, and hope */
}

/* Which connector should we be on right now? External beats internal.
 * `probe` forces a fresh detection (EDID read) - only needed at start-up;
 * after a hotplug uevent the kernel has already probed. */
static uint32_t choose_connector(bool probe, bool * external)
{
    drmModeRes * res = drmModeGetResources(drm_fd);
    if(!res) return 0;
    uint32_t internal = 0, ext = 0;
    for(int i = 0; i < res->count_connectors; i++) {
        drmModeConnector * c = probe ? drmModeGetConnector(drm_fd, res->connectors[i])
                                     : drmModeGetConnectorCurrent(drm_fd, res->connectors[i]);
        if(!c) continue;
        if(c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
            if(is_external(c->connector_type)) { if(!ext) ext = c->connector_id; }
            else if(!internal) internal = c->connector_id;
        }
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);
    *external = ext != 0;
    return ext ? ext : internal;
}

/* Every CRTC this connector's encoders say they can reach, the one it is
 * already on first. The hardware's own list is optimistic on RK3566 (29 Sep:
 * DSI chose the CRTC HDMI had just left and every commit came back EINVAL),
 * so these are only candidates - display_init() test-commits each one. */
static int crtc_candidates(drmModeRes * res, drmModeConnector * c, int * idx, int max)
{
    int n = 0;
    uint32_t seen = 0;
    if(c->encoder_id) {
        drmModeEncoder * e = drmModeGetEncoder(drm_fd, c->encoder_id);
        if(e) {
            for(int i = 0; e->crtc_id && i < res->count_crtcs; i++)
                if(res->crtcs[i] == e->crtc_id) { idx[n++] = i; seen |= 1u << i; }
            drmModeFreeEncoder(e);
        }
    }
    for(int k = 0; k < c->count_encoders; k++) {
        drmModeEncoder * e = drmModeGetEncoder(drm_fd, c->encoders[k]);
        if(!e) continue;
        for(int i = 0; i < res->count_crtcs && n < max; i++)
            if((e->possible_crtcs & (1u << i)) && !(seen & (1u << i))) { idx[n++] = i; seen |= 1u << i; }
        drmModeFreeEncoder(e);
    }
    return n;
}

/* Planes that can show XRGB8888 on this CRTC: primary planes first, then
 * the rest. */
static int plane_candidates(int crtc_idx, uint32_t * out, int max)
{
    drmModePlaneRes * pr = drmModeGetPlaneResources(drm_fd);
    if(!pr) return 0;
    int n = 0;
    for(int pass = 0; pass < 2; pass++) {
        for(uint32_t i = 0; i < pr->count_planes && n < max; i++) {
            drmModePlane * p = drmModeGetPlane(drm_fd, pr->planes[i]);
            if(!p) continue;
            bool fmt = false;
            for(uint32_t f = 0; f < p->count_formats; f++) if(p->formats[f] == DRM_FORMAT_XRGB8888) fmt = true;
            bool primary = prop_value(p->plane_id, DRM_MODE_OBJECT_PLANE, "type") == DRM_PLANE_TYPE_PRIMARY;
            if(fmt && (p->possible_crtcs & (1u << crtc_idx)) && primary == (pass == 0)) out[n++] = p->plane_id;
            drmModeFreePlane(p);
        }
    }
    drmModeFreePlaneResources(pr);
    return n;
}

static void use_plane(uint32_t id)
{
    plane_id = id;
    p_fb = prop_id(id, DRM_MODE_OBJECT_PLANE, "FB_ID");
    p_crtc = prop_id(id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    p_sx = prop_id(id, DRM_MODE_OBJECT_PLANE, "SRC_X");
    p_sy = prop_id(id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    p_sw = prop_id(id, DRM_MODE_OBJECT_PLANE, "SRC_W");
    p_sh = prop_id(id, DRM_MODE_OBJECT_PLANE, "SRC_H");
    p_cx = prop_id(id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    p_cy = prop_id(id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    p_cw = prop_id(id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    p_ch = prop_id(id, DRM_MODE_OBJECT_PLANE, "CRTC_H");
}

static bool alloc_fb(fbuf_t * b, int w, int h)
{
    struct drm_mode_create_dumb creq = { .width = (uint32_t)w, .height = (uint32_t)h, .bpp = 32 };
    if(drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) return false;
    b->handle = creq.handle;
    b->pitch = creq.pitch;
    b->size = creq.size;
    struct drm_mode_map_dumb mreq = { .handle = creq.handle };
    if(drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq)) return false;
    b->map = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, (off_t)mreq.offset);
    if(b->map == MAP_FAILED) { b->map = NULL; return false; }
    memset(b->map, 0, b->size);
    uint32_t handles[4] = { b->handle }, pitches[4] = { b->pitch }, offsets[4] = { 0 };
    return drmModeAddFB2(drm_fd, (uint32_t)w, (uint32_t)h, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &b->fb, 0) == 0;
}

/* LVGL's default theme sizes its padding by the display's DPI. The old
 * driver set DPI from the panel's physical size; keep that exact value on
 * the panel, and on a TV use the panel's value scaled with the stage, so
 * padding keeps its proportions instead of shrinking to TV-sized DPI. */
static int reference_dpi(int stage)
{
    int dpi = 0;
    drmModeRes * res = drmModeGetResources(drm_fd);
    for(int i = 0; res && i < res->count_connectors && !dpi; i++) {
        drmModeConnector * c = drmModeGetConnectorCurrent(drm_fd, res->connectors[i]);
        if(!c) continue;
        if(!is_external(c->connector_type) && c->mmWidth > 0 && c->count_modes > 0) {
            int hres = c->modes[0].hdisplay, vres = c->modes[0].vdisplay;
            int shortest = hres < vres ? hres : vres;
            int panel_dpi = (hres * 25400 + c->mmWidth * 1000 - 1) / (int)(c->mmWidth * 1000);
            dpi = panel_dpi * stage / shortest;
        }
        drmModeFreeConnector(c);
    }
    if(res) drmModeFreeResources(res);
    return dpi;
}

/* ------------------------------------------------------------ flipping */

static void flip_done(int fd, unsigned seq, unsigned sec, unsigned usec, void * data)
{
    (void)fd; (void)seq; (void)sec; (void)usec; (void)data;
    if(pending) { drmModeAtomicFree(pending); pending = NULL; }
}

/* First commit: light our output and switch every other connector, CRTC
 * and plane off, so only one screen is ever showing anything. */
static void add_modeset(drmModeAtomicReq * req)
{
    drmModeRes * res = switch_others_off ? drmModeGetResources(drm_fd) : NULL;
    if(res) {
        for(int i = 0; i < res->count_connectors; i++)
            if(res->connectors[i] != conn_id)
                add(req, res->connectors[i], DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", 0);
        for(int i = 0; i < res->count_crtcs; i++)
            if(res->crtcs[i] != crtc_id) {
                add(req, res->crtcs[i], DRM_MODE_OBJECT_CRTC, "ACTIVE", 0);
                add(req, res->crtcs[i], DRM_MODE_OBJECT_CRTC, "MODE_ID", 0);
            }
        drmModeFreeResources(res);
    }
    drmModePlaneRes * pr = switch_others_off ? drmModeGetPlaneResources(drm_fd) : NULL;
    if(pr) {
        for(uint32_t i = 0; i < pr->count_planes; i++)
            if(pr->planes[i] != plane_id) {
                add(req, pr->planes[i], DRM_MODE_OBJECT_PLANE, "FB_ID", 0);
                add(req, pr->planes[i], DRM_MODE_OBJECT_PLANE, "CRTC_ID", 0);
            }
        drmModeFreePlaneResources(pr);
    }
    add(req, conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", crtc_id);
    add(req, crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", mode_blob);
    add(req, crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", 1);
}

static drmModeAtomicReq * build_request(fbuf_t * b, bool modeset)
{
    drmModeAtomicReq * req = drmModeAtomicAlloc();
    if(modeset) add_modeset(req);
    drmModeAtomicAddProperty(req, plane_id, p_fb, b->fb);
    drmModeAtomicAddProperty(req, plane_id, p_crtc, crtc_id);
    drmModeAtomicAddProperty(req, plane_id, p_sx, 0);
    drmModeAtomicAddProperty(req, plane_id, p_sy, 0);
    drmModeAtomicAddProperty(req, plane_id, p_sw, (uint64_t)mode.hdisplay << 16);
    drmModeAtomicAddProperty(req, plane_id, p_sh, (uint64_t)mode.vdisplay << 16);
    drmModeAtomicAddProperty(req, plane_id, p_cx, 0);
    drmModeAtomicAddProperty(req, plane_id, p_cy, 0);
    drmModeAtomicAddProperty(req, plane_id, p_cw, mode.hdisplay);
    drmModeAtomicAddProperty(req, plane_id, p_ch, mode.vdisplay);
    return req;
}

/* Would the kernel accept this CRTC + plane for our first modeset? */
static int test_config(void)
{
    drmModeAtomicReq * req = build_request(&bufs[0], true);
    int ret = drmModeAtomicCommit(drm_fd, req, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    int err = ret ? errno : 0;
    drmModeAtomicFree(req);
    return err;
}

static int commit(fbuf_t * b)
{
    static int failures;
    /* The first commit (the modeset) is blocking and asks for no flip
     * event. It also switches the other CRTCs off, and the kernel refuses an
     * event on a CRTC that was already off and stays off - which is exactly
     * the HDMI CRTC after an unplug, once fbcon has taken the screens back
     * between our exit and exec (29 Sep: EINVAL on every commit). */
    uint32_t flags = modeset_done ? DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK
                                  : DRM_MODE_ATOMIC_ALLOW_MODESET;
    drmModeAtomicReq * req = build_request(b, !modeset_done);

    pthread_mutex_lock(&commit_lock);
    if(viz_owns) { pthread_mutex_unlock(&commit_lock); drmModeAtomicFree(req); ui_front = b; return 0; }   /* the visualizer has the plane: shown when it gives it back */
    int ret = drmModeAtomicCommit(drm_fd, req, flags, NULL);
    pthread_mutex_unlock(&commit_lock);
    if(!ret) ui_front = b;
    if(ret) {
        int err = errno;
        drmModeAtomicFree(req);
        if(failures++ == 0) fprintf(stderr, "simpleton-ui: atomic commit failed: %s\n", strerror(err));
        /* A modeset the kernel keeps refusing won't start working: quit and
         * let systemd start us again rather than retry every frame. */
        if(!modeset_done && failures >= 3) { fprintf(stderr, "simpleton-ui: giving up on this output\n"); exit(1); }
        return ret;
    }
    failures = 0;
    if(!modeset_done) {                 /* blocking: already on screen, nothing to wait for */
        modeset_done = true;
        drmModeAtomicFree(req);
        return 0;
    }
    pending = req;
    return 0;
}

static void refr_start_cb(lv_event_t * e)
{
    lv_display_t * disp = lv_event_get_current_target(e);
    if(act_buf) return;
    lv_draw_buf_t * db = lv_display_get_buf_active(disp);
    for(int i = 0; i < 2; i++) if(db->unaligned_data == bufs[i].map) act_buf = &bufs[i];
}

static void flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * px)
{
    (void)area; (void)px;
    if(!lv_display_flush_is_last(disp)) return;
    if(act_buf) commit(act_buf);
    act_buf = NULL;
}

static void flush_wait_cb(lv_display_t * disp)
{
    (void)disp;
    struct pollfd pfd = { .fd = drm_fd, .events = POLLIN };
    while(pending) {
        int r = poll(&pfd, 1, 1000);
        if(r > 0) drmHandleEvent(drm_fd, &evctx);
        else if(r == 0) { drmModeAtomicFree(pending); pending = NULL; }   /* never hang the UI on a lost event */
        else if(errno != EINTR) return;
    }
}

/* ------------------------------------------------------------ hotplug */

static void hotplug_open(void)
{
    uevent_fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
    if(uevent_fd < 0) return;
    struct sockaddr_nl a = { .nl_family = AF_NETLINK, .nl_groups = 1 };   /* kernel broadcast group */
    if(bind(uevent_fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(uevent_fd); uevent_fd = -1; }
}

int display_hotplug_fd(void) { return uevent_fd; }

bool display_hotplug_poll(void)
{
    if(uevent_fd >= 0) {
        char buf[4096];
        ssize_t n;
        while((n = recv(uevent_fd, buf, sizeof(buf) - 1, 0)) > 0) {
            buf[n] = 0;
            bool drm = false, hotplug = false;
            for(char * s = buf; s < buf + n; s += strlen(s) + 1) {
                if(strcmp(s, "SUBSYSTEM=drm") == 0) drm = true;
                if(strcmp(s, "HOTPLUG=1") == 0) hotplug = true;
            }
            if(drm && hotplug) {
                if(!settle_at) fprintf(stderr, "simpleton-ui: display hotplug event\n");
                settle_at = now_ms() + HOTPLUG_SETTLE_MS;
            }
        }
    }

    /* Backstop while on the panel: look at the HDMI port every couple of
     * seconds in case the plug-in never produces an event. A full probe of
     * a disconnected port is only a pin read; EDID is read only once
     * something is there, and then we move to it anyway. */
    uint32_t now = now_ms();
    if(!on_external && !settle_at && (int32_t)(now - next_port_check) >= 0) {
        next_port_check = now + PORT_CHECK_MS;
        bool ext;
        uint32_t want = choose_connector(true, &ext);
        if(ext && want && want != conn_id) {
            fprintf(stderr, "simpleton-ui: HDMI found by port check\n");
            settle_at = now + HOTPLUG_SETTLE_MS;
        }
    }

    if(!settle_at || (int32_t)(now - settle_at) < 0) return false;
    settle_at = 0;

    /* Full probe, not the cached state: after an unplug the kernel drops
     * the HDMI port's mode list and only refills it when asked (29 Sep:
     * re-plugging was ignored because the port looked connected with no
     * modes). */
    bool ext;
    uint32_t want = choose_connector(true, &ext);
    if(!want || want == conn_id) return false;
    fprintf(stderr, "simpleton-ui: outputs changed, moving to connector %u\n", want);
    return true;
}

/* ------------------------------------------------------------ the visualizer's plane (0.31) */

/* The planes on our CRTC that take XRGB8888, aren't the UI's and aren't
 * busy on another live CRTC. On the RK3566 (18 Oct, from
 * /sys/kernel/debug/dri/0/state) that is Esmart0 - the panel port's own
 * primary, free while we are on HDMI, and it scales - and Cluster0, which
 * refuses plain buffers (it wants the compressed AFBC layout). */
static int viz_plane_candidates(uint32_t * out, int max)
{
    drmModeRes * res = drmModeGetResources(drm_fd);
    if(!res) return 0;
    int crtc_idx = -1;
    for(int i = 0; i < res->count_crtcs; i++) if(res->crtcs[i] == crtc_id) crtc_idx = i;
    drmModeFreeResources(res);
    if(crtc_idx < 0) return 0;
    drmModePlaneRes * pr = drmModeGetPlaneResources(drm_fd);
    if(!pr) return 0;
    int n = 0;
    for(uint32_t i = 0; i < pr->count_planes && n < max; i++) {
        drmModePlane * p = drmModeGetPlane(drm_fd, pr->planes[i]);
        if(!p) continue;
        bool fmt = false;
        for(uint32_t f = 0; f < p->count_formats; f++) if(p->formats[f] == DRM_FORMAT_XRGB8888) fmt = true;
        uint64_t type = prop_value(p->plane_id, DRM_MODE_OBJECT_PLANE, "type");
        bool busy_elsewhere = p->crtc_id != 0 && p->crtc_id != crtc_id;
        if(fmt && p->plane_id != plane_id && type != DRM_PLANE_TYPE_CURSOR && !busy_elsewhere && (p->possible_crtcs & (1u << crtc_idx))) out[n++] = p->plane_id;
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(pr);
    return n;
}

static drmModeAtomicReq * viz_request(const display_vizbuf_t * b)
{
    drmModeAtomicReq * req = drmModeAtomicAlloc();
    add(req, viz_plane, DRM_MODE_OBJECT_PLANE, "FB_ID", b ? b->fb : 0);
    add(req, viz_plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID", b ? crtc_id : 0);
    if(b) {
        add(req, viz_plane, DRM_MODE_OBJECT_PLANE, "SRC_X", 0);
        add(req, viz_plane, DRM_MODE_OBJECT_PLANE, "SRC_Y", 0);
        add(req, viz_plane, DRM_MODE_OBJECT_PLANE, "SRC_W", (uint64_t)b->w << 16);
        add(req, viz_plane, DRM_MODE_OBJECT_PLANE, "SRC_H", (uint64_t)b->h << 16);
        add(req, viz_plane, DRM_MODE_OBJECT_PLANE, "CRTC_X", 0);
        add(req, viz_plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y", 0);
        add(req, viz_plane, DRM_MODE_OBJECT_PLANE, "CRTC_W", mode.hdisplay);
        add(req, viz_plane, DRM_MODE_OBJECT_PLANE, "CRTC_H", mode.vdisplay);
        if(viz_zpos >= 0) add(req, viz_plane, DRM_MODE_OBJECT_PLANE, "zpos", (uint64_t)viz_zpos);
    }
    return req;
}

static int viz_test(const display_vizbuf_t * b)
{
    drmModeAtomicReq * req = viz_request(b);
    pthread_mutex_lock(&commit_lock);
    int ret = drmModeAtomicCommit(drm_fd, req, DRM_MODE_ATOMIC_TEST_ONLY, NULL);
    int err = ret ? errno : 0;
    pthread_mutex_unlock(&commit_lock);
    drmModeAtomicFree(req);
    return err;
}

/* Which plane, which zpos, and does it scale: found by test-only commits
 * (0.31a - the first try on the unit got EINVAL from the one plane it
 * picked: planes differ in what they take, and VOP2 wants each zpos used
 * once). The UI's plane stays where it is; ours must sit above it. */
static bool viz_probe(void)
{
    if(viz_probed) return viz_ok;
    viz_probed = true;
    uint32_t cands[16]; int nc = viz_plane_candidates(cands, 16);
    if(!nc) { fprintf(stderr, "simpleton-ui: viz: no second plane on this output\n"); return false; }
    int ui_z = (int)prop_value(plane_id, DRM_MODE_OBJECT_PLANE, "zpos");
    int sizes[2][2] = { { mode.hdisplay / 2, mode.vdisplay / 2 }, { mode.hdisplay, mode.vdisplay } };
    for(int si = 0; si < 2 && !viz_ok; si++) {
        display_vizbuf_t t; memset(&t, 0, sizeof(t)); t.dmabuf_fd = -1;
        fbuf_t f;
        if(!alloc_fb(&f, sizes[si][0], sizes[si][1])) continue;
        munmap(f.map, f.size);
        t.w = sizes[si][0]; t.h = sizes[si][1]; t.pitch = f.pitch; t.fb = f.fb; t.handle = f.handle;
        for(int ci = 0; ci < nc && !viz_ok; ci++) {
            viz_plane = cands[ci];
            uint32_t zid = prop_id(viz_plane, DRM_MODE_OBJECT_PLANE, "zpos");
            int zmin = -1, zmax = -1; bool zset = false;
            if(zid) { drmModePropertyRes * zp = drmModeGetProperty(drm_fd, zid); if(zp) { zset = !(zp->flags & DRM_MODE_PROP_IMMUTABLE) && zp->count_values == 2; zmin = (int)zp->values[0]; zmax = (int)zp->values[1]; drmModeFreeProperty(zp); } }
            int own_z = zid ? (int)prop_value(viz_plane, DRM_MODE_OBJECT_PLANE, "zpos") : -1;
            /* as it is (if that already sits above the UI), then every zpos above the UI's, highest first */
            int tries[20]; int nt = 0;
            if(own_z > ui_z || !zid) tries[nt++] = -1;
            if(zset) for(int z = zmax; z > ui_z && z >= zmin && nt < 20; z--) tries[nt++] = z;
            for(int ti = 0; ti < nt && !viz_ok; ti++) {
                viz_zpos = tries[ti];
                int err = viz_test(&t);
                if(!err) { viz_ok = true; viz_scales = si == 0; }
                else fprintf(stderr, "simpleton-ui: viz: plane %u zpos %d %dx%d refused (%s)\n", viz_plane, viz_zpos, t.w, t.h, strerror(err));
            }
        }
        drmModeRmFB(drm_fd, f.fb);
        { struct drm_mode_destroy_dumb d = { .handle = f.handle }; drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d); }
    }
    if(viz_ok) fprintf(stderr, "simpleton-ui: viz: plane %u, zpos %d (the UI's plane %u is at %d), %s\n", viz_plane, viz_zpos, plane_id, ui_z, viz_scales ? "scales a half-size buffer" : "full size only");
    else {
        /* no spare plane will do (the RK3566 on HDMI: Esmart0 belongs to the
         * panel port, Cluster0 wants AFBC): the visualizer borrows the UI's
         * own plane while it runs; the UI's flips are held and the UI comes
         * back when it ends */
        viz_plane = plane_id; viz_zpos = -1;
        for(int si = 0; si < 2 && !viz_ok; si++) {
            display_vizbuf_t t; memset(&t, 0, sizeof(t)); t.dmabuf_fd = -1;
            fbuf_t f;
            if(!alloc_fb(&f, sizes[si][0], sizes[si][1])) continue;
            munmap(f.map, f.size);
            t.w = sizes[si][0]; t.h = sizes[si][1]; t.pitch = f.pitch; t.fb = f.fb; t.handle = f.handle;
            int err = viz_test(&t);
            if(!err) { viz_ok = true; viz_takeover = true; viz_scales = si == 0; }
            else fprintf(stderr, "simpleton-ui: viz: the UI's plane %u %dx%d refused too (%s)\n", viz_plane, t.w, t.h, strerror(err));
            drmModeRmFB(drm_fd, f.fb);
            { struct drm_mode_destroy_dumb d = { .handle = f.handle }; drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d); }
        }
        if(viz_ok) fprintf(stderr, "simpleton-ui: viz: no spare plane - borrowing the UI's plane %u, %s\n", viz_plane, viz_scales ? "scaled from half size" : "full size");
        else { viz_plane = 0; fprintf(stderr, "simpleton-ui: viz: no plane accepted a buffer at all\n"); }
    }
    return viz_ok;
}

bool display_viz_scales(void) { return viz_scales; }

bool display_viz_available(void)
{
    if(!modeset_done) return false;
    return viz_probe();
}

bool display_viz_alloc(display_vizbuf_t * b, int w, int h)
{
    memset(b, 0, sizeof(*b)); b->dmabuf_fd = -1;
    if(!display_viz_available()) return false;
    fbuf_t f;
    if(!alloc_fb(&f, w, h)) return false;
    munmap(f.map, f.size);                       /* the CPU never touches it */
    b->w = w; b->h = h; b->pitch = f.pitch; b->fb = f.fb; b->handle = f.handle;
    if(drmPrimeHandleToFD(drm_fd, f.handle, DRM_CLOEXEC | DRM_RDWR, &b->dmabuf_fd)) { b->dmabuf_fd = -1; display_viz_free(b); return false; }
    int err = viz_test(b);
    if(err) { fprintf(stderr, "simpleton-ui: viz: plane %u refused a %dx%d buffer (%s)\n", viz_plane, w, h, strerror(err)); display_viz_free(b); return false; }
    return true;
}

void display_viz_free(display_vizbuf_t * b)
{
    if(b->dmabuf_fd >= 0) close(b->dmabuf_fd);
    if(b->fb) drmModeRmFB(drm_fd, b->fb);
    if(b->handle) { struct drm_mode_destroy_dumb d = { .handle = b->handle }; drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d); }
    memset(b, 0, sizeof(*b)); b->dmabuf_fd = -1;
}

bool display_viz_show(const display_vizbuf_t * b)
{
    if(!viz_plane || !modeset_done) return false;
    drmModeAtomicReq * req = viz_request(b);
    pthread_mutex_lock(&commit_lock);
    if(viz_takeover) viz_owns = true;
    int ret = drmModeAtomicCommit(drm_fd, req, 0, NULL);          /* blocking: back when it's on screen */
    int err = errno;
    pthread_mutex_unlock(&commit_lock);
    drmModeAtomicFree(req);
    static int failures;
    if(ret && failures++ % 100 == 0) fprintf(stderr, "simpleton-ui: viz plane commit failed: %s\n", strerror(err));
    if(!ret) failures = 0;
    return ret == 0;
}

void display_viz_hide(void)
{
    if(!viz_plane || !modeset_done) return;
    pthread_mutex_lock(&commit_lock);
    if(viz_takeover) {
        /* the UI's plane goes back to the UI's last buffer */
        viz_owns = false;
        fbuf_t * b = ui_front ? ui_front : &bufs[0];
        drmModeAtomicReq * req = build_request(b, false);
        drmModeAtomicCommit(drm_fd, req, 0, NULL);
        drmModeAtomicFree(req);
    }
    else {
        drmModeAtomicReq * req = viz_request(NULL);
        drmModeAtomicCommit(drm_fd, req, 0, NULL);
        drmModeAtomicFree(req);
    }
    pthread_mutex_unlock(&commit_lock);
}

/* ------------------------------------------------------------ public */

lv_display_t * display_init(display_info_t * info)
{
    memset(info, 0, sizeof(*info));
    lv_tick_set_cb(now_ms);

    char * path = NULL;
    drmDevice * devs[8];
    int nd = drmGetDevices2(0, devs, 8);
    for(int i = 0; i < nd && !path; i++)
        if(devs[i]->available_nodes & (1 << DRM_NODE_PRIMARY)) path = strdup(devs[i]->nodes[DRM_NODE_PRIMARY]);
    if(nd > 0) drmFreeDevices(devs, nd);
    if(!path) path = strdup("/dev/dri/card0");

    drm_fd = open(path, O_RDWR | O_CLOEXEC);
    if(drm_fd < 0) { fprintf(stderr, "simpleton-ui: can't open %s\n", path); free(path); return NULL; }
    free(path);
    if(drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) ||
       drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
        fprintf(stderr, "simpleton-ui: no atomic modesetting\n");
        return NULL;
    }

    bool external;
    conn_id = choose_connector(true, &external);
    if(!conn_id) { fprintf(stderr, "simpleton-ui: no connected output\n"); return NULL; }

    drmModeRes * res = drmModeGetResources(drm_fd);
    drmModeConnector * c = drmModeGetConnector(drm_fd, conn_id);
    if(!res || !c) return NULL;
    int mi = external ? pick_external_mode(c) : 0;
    mode = c->modes[mi];
    conn_name(c, info->name, sizeof(info->name));
    int crtcs[16];
    int n_crtc = crtc_candidates(res, c, crtcs, 16);
    drmModeFreeConnector(c);
    if(drmModeCreatePropertyBlob(drm_fd, &mode, sizeof(mode), &mode_blob)) { drmModeFreeResources(res); return NULL; }

    int w = mode.hdisplay, h = mode.vdisplay;
    if(!alloc_fb(&bufs[0], w, h) || !alloc_fb(&bufs[1], w, h)) {
        fprintf(stderr, "simpleton-ui: framebuffer allocation failed\n");
        drmModeFreeResources(res);
        return NULL;
    }

    /* First CRTC + plane pair the kernel accepts, checked with a test-only
     * commit of the real modeset - nothing reaches the screen until one passes. */
    bool found = false;
    for(int pass = 0; pass < 2 && !found; pass++) {
        switch_others_off = pass == 0;
        if(pass == 1) fprintf(stderr, "simpleton-ui: retrying without switching the other outputs off\n");
        for(int i = 0; i < n_crtc && !found; i++) {
            crtc_id = res->crtcs[crtcs[i]];
            uint32_t planes[32];
            int n_plane = plane_candidates(crtcs[i], planes, 32);
            for(int k = 0; k < n_plane && !found; k++) {
                use_plane(planes[k]);
                int err = test_config();
                if(!err) found = true;
                else fprintf(stderr, "simpleton-ui: %s: crtc %u + plane %u refused (%s)\n",
                             info->name, crtc_id, plane_id, strerror(err));
            }
        }
    }
    drmModeFreeResources(res);
    if(!found) { fprintf(stderr, "simpleton-ui: no working CRTC/plane for %s\n", info->name); return NULL; }
    fprintf(stderr, "simpleton-ui: %s on crtc %u, plane %u%s\n", info->name, crtc_id, plane_id,
            switch_others_off ? "" : " (other outputs left as they were)");

    memset(&evctx, 0, sizeof(evctx));
    evctx.version = DRM_EVENT_CONTEXT_VERSION;
    evctx.page_flip_handler = flip_done;

    lv_display_t * disp = lv_display_create(w, h);
    if(!disp) return NULL;
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_flush_wait_cb(disp, flush_wait_cb);
    lv_display_set_buffers_with_stride(disp, bufs[1].map, bufs[0].map, (uint32_t)bufs[0].size,
                                       bufs[0].pitch, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_add_event_cb(disp, refr_start_cb, LV_EVENT_REFR_START, NULL);

    info->w = w;
    info->h = h;
    info->stage = w < h ? w : h;
    int dpi = reference_dpi(info->stage);
    if(dpi > 0) lv_display_set_dpi(disp, dpi);
    info->external = external;
    on_external = external;
    next_port_check = now_ms() + PORT_CHECK_MS;
    fprintf(stderr, "simpleton-ui: %s %dx%d@%u, stage %d\n", info->name, w, h, mode.vrefresh, info->stage);

    hotplug_open();
    return disp;
}
