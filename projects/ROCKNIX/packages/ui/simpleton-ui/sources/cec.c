/*
 * SimpletonOS UI - HDMI-CEC remote input (implementation). See cec.h.
 *
 * Kernel CEC framework in brief:
 *   - CEC_S_MODE initiator+follower: we can send, and we receive every message
 *     the kernel core doesn't answer itself. The core already answers the
 *     identity polls (Give Physical Address, Give OSD Name, CEC Version,
 *     Vendor ID), so we never see those.
 *   - CEC_ADAP_S_LOG_ADDRS: tell the kernel what we are. It claims a logical
 *     address (Playback 1/2/3) whenever a valid physical address appears, i.e.
 *     whenever HDMI is plugged into a CEC-capable TV, and re-claims by itself
 *     after replugs. The setting outlives our fd, so after the UI restarts on
 *     an HDMI hotplug we find it already configured and leave it alone.
 *   - State changes arrive as events (POLLPRI); messages, and the results of
 *     our own non-blocking transmits, arrive via CEC_RECEIVE (POLLIN). Every
 *     open gets an initial state-change event, so "claimed" is handled in one
 *     place whether we just configured the adapter or found it configured.
 *
 * Re-claiming (verified on hardware 30 Sep 2026): the RGB20SX has no pull-up
 * of its own on the CEC line and relies on the TV's. With the TV's CEC setting
 * off the line is dead, the claim fails (tx_status 0x30), and the kernel does
 * not try again until the next HDMI replug. So while we have a physical
 * address but no logical one, we re-issue the claim ourselves: every 5 s for
 * the first minute, then every 30 s. Switching CEC on at the TV is then enough.
 *
 * We deliberately do NOT set CEC_LOG_ADDRS_FL_ALLOW_RC_PASSTHRU: with it the
 * kernel would also turn remote keys into an rc input device, and keys would
 * risk arriving twice. Keys are translated here instead.
 *
 * Held keys (1 Oct 2026): the CEC spec has the TV resend User Control Pressed
 * while a key is held and send User Control Released when it is let go, but
 * TVs differ: some resend every 200 ms, some every half second, some only
 * once. So a held navigation key is no longer one action per TV message -
 * the first press starts the UI's own repeater (input_hold_begin), every
 * resend only confirms the key is still down, and Released stops it. Scroll
 * speed is then the same on every TV, and the same as the D-pad.
 *
 * Messages are built by hand (header byte, opcode, operands) rather than with
 * linux/cec-funcs.h, to depend on nothing beyond linux/cec.h.
 */
#include "cec.h"
#include "input.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/cec.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define CEC_PATH  "/dev/cec0"
#define OSD_NAME  "Simpleton"           /* max 14 chars; shown in the TV's device list */

/* Broadcast Active Source once our address is claimed, so TVs that only
 * forward remote keys to the active source (Samsung Anynet+, some LG) send
 * them to us. This may switch the TV to our input when we come up. It does
 * NOT turn the TV on (that would be Image View On, which we never send).
 * Candidate for a Settings toggle ("Switch TV input when Simpleton starts"). */
#define ANNOUNCE_ON_CLAIM 1

/* Re-claim schedule while HDMI is connected but no logical address is held. */
#define RECLAIM_FAST_MS    5000
#define RECLAIM_SLOW_MS   30000
#define RECLAIM_FAST_TRIES   12          /* 12 x 5 s = the first minute */

/* CEC User Control codes (CEC 1.4 table 27) we act on. */
#define UC_SELECT        0x00
#define UC_UP            0x01
#define UC_DOWN          0x02
#define UC_LEFT          0x03
#define UC_RIGHT         0x04
#define UC_ROOT_MENU     0x09
#define UC_SETUP_MENU    0x0A
#define UC_CONTENTS_MENU 0x0B
#define UC_EXIT          0x0D
#define UC_CONTEXT_MENU  0x11
#define UC_PLAY          0x44
#define UC_STOP          0x45
#define UC_PAUSE         0x46
#define UC_REWIND        0x48
#define UC_FAST_FORWARD  0x49
#define UC_FORWARD       0x4B
#define UC_BACKWARD      0x4C
#define UC_PLAY_FUNC     0x60
#define UC_PAUSE_PLAY    0x61

static int fd = -1;
static uint8_t  my_la = 0xF;           /* our logical address, 0xF = none yet */
static uint16_t my_pa = CEC_PHYS_ADDR_INVALID;
static bool active;                     /* we believe we're the active source */

/* Re-claim bookkeeping (see header comment). */
static uint32_t last_claim_ms;
static int claim_tries;

/* Key repeat tracking: a held remote key arrives as repeated "pressed"
 * messages with no "released" in between. */
static int held_code = -1;

int cec_fd(void) { return fd; }

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ---- sending ---- */

static void transmit(uint8_t dest, const uint8_t * body, int len)
{
    if(fd < 0 || my_la == 0xF) return;
    struct cec_msg m;
    memset(&m, 0, sizeof(m));
    m.msg[0] = (uint8_t)((my_la << 4) | (dest & 0xF));
    memcpy(&m.msg[1], body, (size_t)len);
    m.len = (uint32_t)(len + 1);
    /* non-blocking fd: queued now, result comes back through CEC_RECEIVE */
    if(ioctl(fd, CEC_TRANSMIT, &m) < 0 && errno != EAGAIN)
        fprintf(stderr, "simpleton-ui: cec: transmit 0x%02x: %s\n", body[0], strerror(errno));
}

static void send_active_source(void)
{
    if(my_pa == CEC_PHYS_ADDR_INVALID) return;
    uint8_t b[] = { CEC_MSG_ACTIVE_SOURCE, (uint8_t)(my_pa >> 8), (uint8_t)(my_pa & 0xFF) };
    transmit(CEC_LOG_ADDR_BROADCAST, b, sizeof(b));
    active = true;
}

static void send_feature_abort(uint8_t dest, uint8_t opcode)
{
    uint8_t b[] = { CEC_MSG_FEATURE_ABORT, opcode, CEC_OP_ABORT_UNRECOGNIZED_OP };
    transmit(dest, b, sizeof(b));
}

/* ---- setup ---- */

static bool configured_as_us(const struct cec_log_addrs * la)
{
    return la->num_log_addrs == 1 &&
           la->log_addr_type[0] == CEC_LOG_ADDR_TYPE_PLAYBACK &&
           strncmp(la->osd_name, OSD_NAME, sizeof(la->osd_name)) == 0;
}

static void fill_log_addrs(struct cec_log_addrs * la)
{
    memset(la, 0, sizeof(*la));
    la->cec_version = CEC_OP_CEC_VERSION_1_4;   /* widest TV compatibility */
    la->vendor_id = CEC_VENDOR_ID_NONE;
    la->num_log_addrs = 1;
    la->log_addr_type[0] = CEC_LOG_ADDR_TYPE_PLAYBACK;
    la->primary_device_type[0] = CEC_OP_PRIM_DEVTYPE_PLAYBACK;
    la->all_device_types[0] = CEC_OP_ALL_DEVTYPE_PLAYBACK;
    snprintf(la->osd_name, sizeof(la->osd_name), "%s", OSD_NAME);
}

/* Clear and set again: the kernel starts a fresh claim in the background
 * (fd is non-blocking) and reports the outcome as a state-change event.
 * EBUSY means a claim is already running, which is fine. */
static void claim(void)
{
    struct cec_log_addrs la;
    memset(&la, 0, sizeof(la));
    ioctl(fd, CEC_ADAP_S_LOG_ADDRS, &la);           /* clear: sends nothing on the bus */
    fill_log_addrs(&la);
    if(ioctl(fd, CEC_ADAP_S_LOG_ADDRS, &la) < 0 && errno != EAGAIN && errno != EBUSY)
        fprintf(stderr, "simpleton-ui: cec: S_LOG_ADDRS: %s\n", strerror(errno));
}

bool cec_init(void)
{
    fd = open(CEC_PATH, O_RDWR | O_CLOEXEC);
    if(fd < 0) {
        fprintf(stderr, "simpleton-ui: cec: no adapter (%s: %s)\n", CEC_PATH, strerror(errno));
        return false;
    }

    struct cec_caps caps;
    memset(&caps, 0, sizeof(caps));
    if(ioctl(fd, CEC_ADAP_G_CAPS, &caps) < 0 || !(caps.capabilities & CEC_CAP_LOG_ADDRS)) {
        fprintf(stderr, "simpleton-ui: cec: adapter can't take logical addresses, CEC off\n");
        close(fd); fd = -1;
        return false;
    }
    fprintf(stderr, "simpleton-ui: cec: adapter %s (%s), caps 0x%x\n", caps.name, caps.driver, caps.capabilities);

    uint32_t mode = CEC_MODE_INITIATOR | CEC_MODE_FOLLOWER;
    if(ioctl(fd, CEC_S_MODE, &mode) < 0) {
        fprintf(stderr, "simpleton-ui: cec: S_MODE: %s\n", strerror(errno));
        close(fd); fd = -1;
        return false;
    }

    /* Non-blocking from here on: claims happen in the background (they need
     * the TV), and we hear about them as state-change events. */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

    struct cec_log_addrs la;
    memset(&la, 0, sizeof(la));
    ioctl(fd, CEC_ADAP_G_LOG_ADDRS, &la);
    if(!configured_as_us(&la)) claim();

    last_claim_ms = now_ms();
    claim_tries = 0;
    return true;
}

/* ---- events: address claimed / lost ---- */

static void on_state_change(const struct cec_event * ev)
{
    uint16_t mask = ev->state_change.log_addr_mask;
    uint16_t pa = ev->state_change.phys_addr;
    uint8_t la = 0xF;
    for(int i = 0; i < 15; i++) if(mask & (1u << i)) { la = (uint8_t)i; break; }

    /* Our own clear-and-set produces a "nothing claimed" event in between;
     * only log real changes so the 5 s retries stay quiet. */
    if(la != my_la || pa != my_pa)
        fprintf(stderr, "simpleton-ui: cec: state: phys %x.%x.%x.%x, logical %d\n",
                pa >> 12, (pa >> 8) & 0xF, (pa >> 4) & 0xF, pa & 0xF, la == 0xF ? -1 : la);

    if(pa != my_pa) {                    /* plugged / unplugged / new TV input */
        last_claim_ms = now_ms();
        claim_tries = 0;
    }

    bool newly_claimed = la != 0xF && pa != CEC_PHYS_ADDR_INVALID &&
                         (la != my_la || pa != my_pa);
    my_la = la;
    my_pa = pa;
    held_code = -1;
    input_hold_end();
    if(la == 0xF) active = false;
    if(newly_claimed && ANNOUNCE_ON_CLAIM) send_active_source();
}

/* ---- remote keys ---- */

static void on_key_pressed(uint8_t code)
{
    bool repeat = (code == held_code);
    held_code = code;

    ui_action_t a = ACT_NONE;
    bool repeats = false;                /* only navigation auto-repeats */
    switch(code) {
        case UC_UP:            a = ACT_UP;    repeats = true; break;
        case UC_DOWN:          a = ACT_DOWN;  repeats = true; break;
        case UC_LEFT:          a = ACT_LEFT;  repeats = true; break;
        case UC_RIGHT:         a = ACT_RIGHT; repeats = true; break;
        case UC_SELECT:        a = ACT_SELECT;    break;
        case UC_EXIT:          a = ACT_BACK;      break;
        case UC_ROOT_MENU:     a = ACT_HOME;      break;   /* TVs often keep Home for themselves */
        case UC_SETUP_MENU:
        case UC_CONTENTS_MENU:
        case UC_CONTEXT_MENU:  a = ACT_MENU;      break;   /* remotes have no long-press: own key */
        case UC_PLAY:
        case UC_PLAY_FUNC:     a = ACT_PLAY;      break;
        case UC_PAUSE:
        case UC_STOP:          a = ACT_PAUSE;     break;   /* stop = pause: keep the position */
        case UC_PAUSE_PLAY:    a = ACT_PLAYPAUSE; break;
        case UC_FORWARD:
        case UC_FAST_FORWARD:  a = ACT_NEXT;      break;
        case UC_BACKWARD:
        case UC_REWIND:        a = ACT_PREV;      break;
        default:
            if(!repeat) fprintf(stderr, "simpleton-ui: cec: unmapped remote key 0x%02x\n", code);
            return;
    }
    if(repeats) {
        /* navigation: the UI's repeater sets the pace, the TV only says
         * "still held" (see header) */
        if(repeat) input_hold_refresh(a); else input_hold_begin(a);
        return;
    }
    if(repeat) return;                   /* a held transport key fires once */
    input_push(a);
}

/* ---- incoming messages ---- */

static void on_message(const struct cec_msg * m)
{
    if(m->len < 2) return;                          /* poll message, no opcode */
    uint8_t from = m->msg[0] >> 4;
    uint8_t to = m->msg[0] & 0xF;
    uint8_t op = m->msg[1];
    bool broadcast = (to == CEC_LOG_ADDR_BROADCAST);

    switch(op) {
        case CEC_MSG_USER_CONTROL_PRESSED:
            if(m->len >= 3) on_key_pressed(m->msg[2]);
            return;
        case CEC_MSG_USER_CONTROL_RELEASED:
            held_code = -1;
            input_hold_end();
            return;

        case CEC_MSG_REQUEST_ACTIVE_SOURCE:         /* "who's on screen?" */
            if(active) send_active_source();
            return;
        case CEC_MSG_SET_STREAM_PATH:               /* TV picked an input */
            if(m->len >= 4) {
                uint16_t pa = (uint16_t)((m->msg[2] << 8) | m->msg[3]);
                if(pa == my_pa) send_active_source(); else active = false;
            }
            return;
        case CEC_MSG_ROUTING_CHANGE:                /* new path in msg[4..5] */
            if(m->len >= 6) {
                uint16_t pa = (uint16_t)((m->msg[4] << 8) | m->msg[5]);
                if(pa == my_pa) send_active_source(); else active = false;
            }
            return;
        case CEC_MSG_ACTIVE_SOURCE:                 /* someone else took over */
            if(from != my_la) active = false;
            return;

        case CEC_MSG_GIVE_DEVICE_POWER_STATUS: {
            uint8_t b[] = { CEC_MSG_REPORT_POWER_STATUS, CEC_OP_POWER_STATUS_ON };
            transmit(from, b, sizeof(b));
            return;
        }
        case CEC_MSG_MENU_REQUEST: {
            /* LG and others only forward keys while our "menu" is active.
             * Ours always is: the UI is the menu. */
            uint8_t b[] = { CEC_MSG_MENU_STATUS, CEC_OP_MENU_STATE_ACTIVATED };
            transmit(from, b, sizeof(b));
            return;
        }

        case CEC_MSG_STANDBY:                       /* TV off: music keeps playing */
        case CEC_MSG_FEATURE_ABORT:                 /* never answer an abort */
            return;
    }

    /* Directed message we don't support: the spec wants a Feature Abort.
     * Broadcasts are never answered. */
    if(!broadcast) send_feature_abort(from, op);
}

void cec_poll(void)
{
    if(fd < 0) return;

    for(;;) {
        struct cec_event ev;
        memset(&ev, 0, sizeof(ev));
        if(ioctl(fd, CEC_DQEVENT, &ev) < 0) break;   /* EAGAIN: none pending */
        if(ev.event == CEC_EVENT_STATE_CHANGE) on_state_change(&ev);
        else if(ev.event == CEC_EVENT_LOST_MSGS)
            fprintf(stderr, "simpleton-ui: cec: lost %u messages\n", ev.lost_msgs.lost_msgs);
    }

    for(;;) {
        struct cec_msg m;
        memset(&m, 0, sizeof(m));
        if(ioctl(fd, CEC_RECEIVE, &m) < 0) break;    /* EAGAIN: none pending */
        if(m.tx_status) {
            /* result of one of our own transmits */
            if(!(m.tx_status & CEC_TX_STATUS_OK))
                fprintf(stderr, "simpleton-ui: cec: send 0x%02x failed (tx_status 0x%x)\n",
                        m.len > 1 ? m.msg[1] : 0, m.tx_status);
            continue;
        }
        on_message(&m);
    }

    /* HDMI connected but nothing claimed (TV's CEC was off, or switched on
     * later): try again on the schedule in the header comment. */
    if(my_pa != CEC_PHYS_ADDR_INVALID && my_la == 0xF) {
        uint32_t t = now_ms();
        uint32_t wait = claim_tries < RECLAIM_FAST_TRIES ? RECLAIM_FAST_MS : RECLAIM_SLOW_MS;
        if(t - last_claim_ms >= wait) {
            last_claim_ms = t;
            claim_tries++;
            claim();
        }
    }
}
