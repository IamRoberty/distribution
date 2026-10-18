/*
 * SimpletonOS UI - evdev joypad reader -> abstract actions.
 *
 * Reads the rocknix-singleadc-joypad ("retrogame_joypad") directly. No SDL,
 * no libevdev: the events are a fixed 24-byte struct and we only care about
 * EV_KEY. Codes come from the RGB20SX device tree override
 * (rk3566-powkiddy-rk2023.dtsi): D-pad is four discrete buttons, and the
 * face buttons are the standard layout (A=south, B=east, X=north, Y=west).
 *
 * Also opens the volume rocker (gpio-keys-vol) so the rocker never feels
 * dead: in Fixed-volume mode the UI shows an overlay instead of doing nothing.
 *
 * Known quirk, handled here: BTN_MODE (the F button) is emitted by two
 * drivers (adc-keys and the joypad's sw11). We don't open adc-keys, so the
 * press reaches us once; since 0.21a it is the Home button (Ian, 15 Oct:
 * a favourite button isn't needed, holding A on a track does that).
 *
 * Auto-repeat (1 Oct 2026): one repeater for every producer. A held D-pad
 * direction repeats after REPEAT_DELAY_MS, every REPEAT_RATE_MS, and after
 * REPEAT_ACCEL_MS of holding every REPEAT_FAST_MS. CEC keys use the same
 * clock through input_hold_*(): TVs differ wildly in whether and how often
 * they resend a held key, so the TV only says "held" / "released" and the
 * pace is ours. A remote hold that goes quiet (no release, no repeat) ends
 * itself after HOLD_TIMEOUT_MS.
 *
 * Every other input device (0.26, Ian 17 Oct: Bluetooth controllers,
 * remotes and keyboards; USB ones get the same): opened when it reports
 * keys, watched for through inotify on /dev/input, dropped when it goes
 * (read gives ENODEV). One epoll descriptor carries them all to the main
 * loop's poll(). They speak the same grammar:
 *   a game pad     its buttons as the built-in pad's (A south, B east, X
 *                  north, Y west, Start, Select, Mode); the D-pad as
 *                  buttons or as the hat axes; the left stick as a D-pad
 *                  past 60 % of its travel
 *   a keyboard     arrows; Enter = A (held: the menu); Esc and Backspace =
 *                  Back; Space = play/pause; Page Up/Down = Left/Right
 *                  (pages); Home = the Home page; Tab = Settings; the Menu
 *                  key = the object menu; N / P = next / previous; while the
 *                  on-screen keyboard is up, letters type (US layout) and
 *                  Backspace erases
 *   a media remote play/pause, play, pause, stop, next, previous, volume,
 *                  OK/select, back, home, menu, info (= settings)
 */
#include "input.h"
#include "config.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define JOYPAD_PATH "/dev/input/by-path/platform-rocknix-singleadc-joypad-event-joystick"
#define VOLKEYS_PATH "/dev/input/by-path/platform-gpio-keys-vol-event"

/* Timing, ms */
#define HOLD_MS          450   /* press longer than this = "hold"           */
#ifndef BTN_MODE
#define BTN_MODE         0x13c
#endif
#define REPEAT_DELAY_MS  400   /* D-pad: first repeat after this            */
#define REPEAT_RATE_MS   100   /* D-pad: then every this...                 */
#define REPEAT_ACCEL_MS  1200  /* ...until held this long, then             */
#define REPEAT_FAST_MS   50    /* every this                                */
#define HOLD_TIMEOUT_MS  2000  /* remote hold with no news: let go          */

#define QUEUE_LEN 32

static int joy_fd = -1;
static int vol_fd = -1;

/* ---- every other device (0.26) ---- */
#define EXTRA_MAX   8
#define STICK_ON    60          /* percent of the stick's half-travel: a direction */
#define STICK_OFF   40          /* back under this: let go */
typedef struct {
    int      fd;
    dev_t    rdev;
    char     name[80];
    char     node[24];          /* eventN */
    int      hat_x, hat_y;      /* -1 / 0 / 1 */
    int      stick_x, stick_y;  /* the direction the stick holds, -1 / 0 / 1 */
    int      ax_min[2], ax_max[2];
    bool     has_stick;
} extra_t;
static extra_t extras[EXTRA_MAX];
static int nextra;
static int ep_fd = -1, ino_fd = -1;
static bool rescan_due;

/* text entry */
static bool swap_ab;          /* Settings -> Controls: A and B change places (0.26a) */
static bool text_mode;
static bool shift_l, shift_r, caps;
static char text_buf[64];
static int  text_len;

static ui_action_t queue[QUEUE_LEN];
static int q_head, q_tail;

/* Per-button press timestamps for hold detection (A and X). */
static uint32_t a_down_ms, x_down_ms;
static bool a_down, x_down;

/* Currently held D-pad direction for software auto-repeat. */
static ui_action_t held_dir = ACT_NONE;
static uint32_t held_since_ms, last_repeat_ms, held_news_ms;
static bool held_remote;                 /* hold came from input_hold_begin() */

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void push(ui_action_t a)
{
    int next = (q_tail + 1) % QUEUE_LEN;
    if(next == q_head) return;           /* full: drop, never block */
    queue[q_tail] = a;
    q_tail = next;
}

void input_push(ui_action_t a) { push(a); }

static void hold_start(ui_action_t dir, uint32_t t, bool remote)
{
    push(dir);
    held_dir = dir;
    held_since_ms = last_repeat_ms = held_news_ms = t;
    held_remote = remote;
}

void input_hold_begin(ui_action_t dir) { hold_start(dir, now_ms(), true); }

void input_hold_refresh(ui_action_t dir)
{
    if(held_dir == dir) held_news_ms = now_ms();
    else hold_start(dir, now_ms(), true);
}

void input_hold_end(void) { held_dir = ACT_NONE; }

ui_action_t input_next_action(void)
{
    if(q_head == q_tail) return ACT_NONE;
    ui_action_t a = queue[q_head];
    q_head = (q_head + 1) % QUEUE_LEN;
    return a;
}

int input_fd(void) { return ep_fd >= 0 ? ep_fd : joy_fd; }

void input_text_mode(bool on) { text_mode = on; text_len = 0; }
const char * input_letter_select(void) { return swap_ab ? "B" : "A"; }
const char * input_letter_back(void)   { return swap_ab ? "A" : "B"; }

void input_reload_settings(void)
{
    swap_ab = config_read_bool("swap_ab", false);
    fprintf(stderr, "simpleton-ui: input: A and B %s\n", swap_ab ? "swapped" : "as labelled");
}
void input_take_text(char * out, int len)
{
    int n = text_len < len - 1 ? text_len : len - 1;
    memcpy(out, text_buf, (size_t)n);
    out[n] = 0;
    text_len = 0;
}

static int open_nonblock(const char * path)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if(fd < 0) fprintf(stderr, "simpleton-ui: input: cannot open %s: %s\n", path, strerror(errno));
    return fd;
}

static void ep_add(int fd)
{
    if(ep_fd < 0 || fd < 0) return;
    struct epoll_event e = { .events = EPOLLIN, .data.fd = fd };
    epoll_ctl(ep_fd, EPOLL_CTL_ADD, fd, &e);
}

static dev_t rdev_of_fd(int fd)
{
    struct stat st;
    return fd >= 0 && fstat(fd, &st) == 0 ? st.st_rdev : 0;
}

#define BITS_LONG     (sizeof(long) * 8)
#define NLONGS(x)     (((x) + BITS_LONG - 1) / BITS_LONG)
static bool test_bit(const unsigned long * b, int bit) { return (b[bit / BITS_LONG] >> (bit % BITS_LONG)) & 1; }

/* Whether a device reports any key we'd act on. */
static bool useful_keys(int fd)
{
    unsigned long ev[NLONGS(EV_MAX + 1)] = { 0 }, keys[NLONGS(KEY_MAX + 1)] = { 0 };
    if(ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev) < 0 || !test_bit(ev, EV_KEY)) return false;
    if(ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0) return false;
    static const int want[] = { KEY_ENTER, KEY_UP, KEY_A, BTN_SOUTH, BTN_GAMEPAD, BTN_DPAD_UP, KEY_PLAYPAUSE, KEY_PLAY,
                                KEY_NEXTSONG, KEY_SELECT, KEY_OK, KEY_BACK, KEY_HOMEPAGE, KEY_VOLUMEUP, KEY_MENU };
    for(unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) if(test_bit(keys, want[i])) return true;
    return false;
}

static void open_extra(const char * node)
{
    if(nextra >= EXTRA_MAX) return;
    char path[64];
    snprintf(path, sizeof(path), "/dev/input/%s", node);
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if(fd < 0) return;
    dev_t rd = rdev_of_fd(fd);
    bool known = rd && (rd == rdev_of_fd(joy_fd) || rd == rdev_of_fd(vol_fd));
    for(int i = 0; i < nextra && !known; i++) known = extras[i].rdev == rd;
    char name[80] = "";
    ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
    /* the power key is power.c's; adc-keys repeats the F button (see above) */
    if(known || strcmp(name, "rk805 pwrkey") == 0 || strcmp(name, "adc-keys") == 0 || !useful_keys(fd)) { close(fd); return; }
    extra_t * x = &extras[nextra++];
    memset(x, 0, sizeof(*x));
    x->fd = fd; x->rdev = rd;
    snprintf(x->name, sizeof(x->name), "%s", name);
    snprintf(x->node, sizeof(x->node), "%s", node);
    unsigned long abs[NLONGS(ABS_MAX + 1)] = { 0 };
    if(ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs) >= 0 && test_bit(abs, ABS_X) && test_bit(abs, ABS_Y)) {
        struct input_absinfo ai;
        if(ioctl(fd, EVIOCGABS(ABS_X), &ai) == 0) { x->ax_min[0] = ai.minimum; x->ax_max[0] = ai.maximum; }
        if(ioctl(fd, EVIOCGABS(ABS_Y), &ai) == 0) { x->ax_min[1] = ai.minimum; x->ax_max[1] = ai.maximum; }
        x->has_stick = x->ax_max[0] > x->ax_min[0] && x->ax_max[1] > x->ax_min[1];
    }
    ep_add(fd);
    fprintf(stderr, "simpleton-ui: input: + \"%s\" (%s)%s\n", name, node, x->has_stick ? ", stick" : "");
}

static void close_extra(int i)
{
    fprintf(stderr, "simpleton-ui: input: - \"%s\" (%s)\n", extras[i].name, extras[i].node);
    if(ep_fd >= 0) epoll_ctl(ep_fd, EPOLL_CTL_DEL, extras[i].fd, NULL);
    close(extras[i].fd);
    extras[i] = extras[--nextra];
}

static void scan_devices(void)
{
    DIR * d = opendir("/dev/input");
    if(!d) return;
    struct dirent * e;
    while((e = readdir(d))) if(strncmp(e->d_name, "event", 5) == 0) open_extra(e->d_name);
    closedir(d);
}

bool input_init(void)
{
    joy_fd = open_nonblock(JOYPAD_PATH);
    vol_fd = open_nonblock(VOLKEYS_PATH);
    input_reload_settings();
    ep_fd = epoll_create1(EPOLL_CLOEXEC);
    ep_add(joy_fd);
    ep_add(vol_fd);
    ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if(ino_fd >= 0 && inotify_add_watch(ino_fd, "/dev/input", IN_CREATE | IN_ATTRIB | IN_DELETE) >= 0) ep_add(ino_fd);
    scan_devices();
    return joy_fd >= 0;
}

static ui_action_t dpad_action(uint16_t code)
{
    switch(code) {
        case BTN_DPAD_UP:    return ACT_UP;
        case BTN_DPAD_DOWN:  return ACT_DOWN;
        case BTN_DPAD_LEFT:  return ACT_LEFT;
        case BTN_DPAD_RIGHT: return ACT_RIGHT;
        default:             return ACT_NONE;
    }
}

static void handle_key(uint16_t code, int32_t value, uint32_t t)
{
    /* value: 1 = press, 0 = release, 2 = kernel autorepeat (not enabled) */
    if(value == 2) return;
    bool pressed = value == 1;
    if(swap_ab) code = code == BTN_SOUTH ? BTN_EAST : code == BTN_EAST ? BTN_SOUTH : code;

    ui_action_t dir = dpad_action(code);
    if(dir != ACT_NONE) {
        if(pressed) hold_start(dir, t, false);
        else if(held_dir == dir && !held_remote) held_dir = ACT_NONE;
        return;
    }

    switch(code) {
        case BTN_SOUTH:                         /* A */
            if(pressed) { a_down = true; a_down_ms = t; }
            else if(a_down) { a_down = false; push(t - a_down_ms >= HOLD_MS ? ACT_MENU : ACT_SELECT); }
            break;
        case BTN_NORTH:                         /* X */
            if(pressed) { x_down = true; x_down_ms = t; }
            else if(x_down) { x_down = false; push(t - x_down_ms >= HOLD_MS ? ACT_PREV : ACT_NEXT); }
            break;
        case BTN_EAST:                          /* B */
            if(pressed) push(ACT_BACK);
            break;
        case BTN_WEST:                          /* Y */
            if(pressed) push(ACT_PLAYPAUSE);
            break;
        case KEY_VOLUMEUP:
            if(pressed) push(ACT_VOL_UP);
            break;
        case KEY_VOLUMEDOWN:
            if(pressed) push(ACT_VOL_DOWN);
            break;
        case BTN_START:
            if(pressed) push(ACT_HOME);
            break;
        case BTN_TL:                            /* 310, 311: L1 / R1 - pages, like Left / Right */
            if(pressed) push(ACT_LEFT);         /* (0.26a; Ian's "L and R scroll the art") */
            break;
        case BTN_TR:
            if(pressed) push(ACT_RIGHT);
            break;
        case BTN_TL2:                           /* 312, 313: rear triggers - */
        case BTN_TR2:                           /* coming off the hardware   */
            break;                              /* (29 Sep), easy to knock   */
                                                /* when unplugging HDMI      */
        case BTN_SELECT:                        /* 314: settings (0.13) */
            if(pressed) push(ACT_SETTINGS);
            break;
        case BTN_MODE:                          /* 316, the F button: Home (0.21a) */
            if(pressed) push(ACT_HOMEPAGE);
            break;
        default:
            /* shoulders, select, F: unmapped. Logged once per press so a
             * button that turns out to use a different code is easy to spot. */
            if(pressed) fprintf(stderr, "simpleton-ui: input: unmapped key code %u\n", code);
            break;
    }
}

/* ---- the other devices' keys ---- */

/* US layout: the keycodes of the printable keys, plain and shifted. */
static char key_char(uint16_t code, bool shifted)
{
    static const struct { uint16_t c; char lo, hi; } map[] = {
        {KEY_1,'1','!'},{KEY_2,'2','@'},{KEY_3,'3','#'},{KEY_4,'4','$'},{KEY_5,'5','%'},{KEY_6,'6','^'},{KEY_7,'7','&'},
        {KEY_8,'8','*'},{KEY_9,'9','('},{KEY_0,'0',')'},{KEY_MINUS,'-','_'},{KEY_EQUAL,'=','+'},
        {KEY_Q,'q','Q'},{KEY_W,'w','W'},{KEY_E,'e','E'},{KEY_R,'r','R'},{KEY_T,'t','T'},{KEY_Y,'y','Y'},{KEY_U,'u','U'},
        {KEY_I,'i','I'},{KEY_O,'o','O'},{KEY_P,'p','P'},{KEY_LEFTBRACE,'[','{'},{KEY_RIGHTBRACE,']','}'},
        {KEY_A,'a','A'},{KEY_S,'s','S'},{KEY_D,'d','D'},{KEY_F,'f','F'},{KEY_G,'g','G'},{KEY_H,'h','H'},{KEY_J,'j','J'},
        {KEY_K,'k','K'},{KEY_L,'l','L'},{KEY_SEMICOLON,';',':'},{KEY_APOSTROPHE,'\'','"'},{KEY_GRAVE,'`','~'},
        {KEY_BACKSLASH,'\\','|'},{KEY_Z,'z','Z'},{KEY_X,'x','X'},{KEY_C,'c','C'},{KEY_V,'v','V'},{KEY_B,'b','B'},
        {KEY_N,'n','N'},{KEY_M,'m','M'},{KEY_COMMA,',','<'},{KEY_DOT,'.','>'},{KEY_SLASH,'/','?'},{KEY_SPACE,' ',' '},
    };
    for(unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if(map[i].c != code) continue;
        bool letter = map[i].lo >= 'a' && map[i].lo <= 'z';
        bool up = letter ? (shifted != caps) : shifted;
        return up ? map[i].hi : map[i].lo;
    }
    return 0;
}

static bool enter_down; static uint32_t enter_down_ms;

static void extra_key(uint16_t code, int32_t value, uint32_t t)
{
    if(code == KEY_LEFTSHIFT)  { shift_l = value != 0; return; }
    if(code == KEY_RIGHTSHIFT) { shift_r = value != 0; return; }
    if(value == 2) {
        /* the keyboard's own repeat: only for typing and erasing (arrows use ours) */
        if(!text_mode) return;
        if(code == KEY_BACKSPACE) { push(ACT_ERASE); return; }
        char ch = key_char(code, shift_l || shift_r);
        if(ch && text_len < (int)sizeof(text_buf) - 1) { text_buf[text_len++] = ch; push(ACT_TEXT); }
        return;
    }
    bool pressed = value == 1;
    if(code == KEY_CAPSLOCK) { if(pressed) caps = !caps; return; }
    if(code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL || code == KEY_LEFTALT || code == KEY_RIGHTALT ||
       code == KEY_LEFTMETA || code == KEY_RIGHTMETA) return;                /* modifiers: nothing, not even a log line */

    /* typing comes first while the on-screen keyboard is up */
    if(text_mode && pressed) {
        if(code == KEY_BACKSPACE) { push(ACT_ERASE); return; }
        char ch = key_char(code, shift_l || shift_r);
        if(ch) { if(text_len < (int)sizeof(text_buf) - 1) { text_buf[text_len++] = ch; push(ACT_TEXT); } return; }
    }

    /* the game-pad buttons, the D-pad buttons: as the built-in pad */
    switch(code) {
        case BTN_SOUTH: case BTN_EAST: case BTN_NORTH: case BTN_WEST: case BTN_START: case BTN_SELECT: case BTN_MODE:
        case BTN_TL: case BTN_TR: case BTN_TL2: case BTN_TR2:
        case BTN_DPAD_UP: case BTN_DPAD_DOWN: case BTN_DPAD_LEFT: case BTN_DPAD_RIGHT:
            handle_key(code, value, t);
            return;
        default: break;
    }

    /* keyboard and remote directions: held, with our repeat */
    ui_action_t dir = code == KEY_UP ? ACT_UP : code == KEY_DOWN ? ACT_DOWN : code == KEY_LEFT ? ACT_LEFT : code == KEY_RIGHT ? ACT_RIGHT : ACT_NONE;
    if(dir != ACT_NONE) {
        if(pressed) hold_start(dir, t, false);
        else if(held_dir == dir && !held_remote) held_dir = ACT_NONE;
        return;
    }
    /* Enter / OK: A - held, the menu */
    if(code == KEY_ENTER || code == KEY_KPENTER || code == KEY_OK || code == KEY_SELECT) {
        if(text_mode && code != KEY_OK && code != KEY_SELECT) { if(pressed) push(ACT_HOME); return; }   /* typing: Enter = done */
        if(pressed) { enter_down = true; enter_down_ms = t; }
        else if(enter_down) { enter_down = false; push(t - enter_down_ms >= HOLD_MS ? ACT_MENU : ACT_SELECT); }
        return;
    }
    if(!pressed) return;
    ui_action_t a = ACT_NONE;
    switch(code) {
        case KEY_ESC: case KEY_BACK: case KEY_BACKSPACE: a = ACT_BACK; break;
        case KEY_SPACE: case KEY_PLAYPAUSE: a = ACT_PLAYPAUSE; break;
        case KEY_PLAY: case KEY_PLAYCD:      a = ACT_PLAY; break;
        case KEY_PAUSE: case KEY_PAUSECD: case KEY_STOP: case KEY_STOPCD: a = ACT_PAUSE; break;
        case KEY_NEXTSONG: case KEY_N:       a = ACT_NEXT; break;
        case KEY_PREVIOUSSONG: case KEY_P:   a = ACT_PREV; break;
        case KEY_VOLUMEUP:                   a = ACT_VOL_UP; break;
        case KEY_VOLUMEDOWN:                 a = ACT_VOL_DOWN; break;
        case KEY_PAGEUP:                     a = ACT_LEFT; break;
        case KEY_PAGEDOWN:                   a = ACT_RIGHT; break;
        case KEY_HOMEPAGE: case KEY_HOME:    a = ACT_HOMEPAGE; break;
        case KEY_MENU: case KEY_COMPOSE: case KEY_CONTEXT_MENU: case KEY_OPTION: a = ACT_MENU; break;
        case KEY_TAB: case KEY_INFO: case KEY_SETUP: a = ACT_SETTINGS; break;
        case KEY_MEDIA: case KEY_AUDIO:      a = ACT_HOME; break;     /* the "now playing" key some remotes have */
        default:
            fprintf(stderr, "simpleton-ui: input: unmapped key code %u\n", code);
            break;
    }
    if(a != ACT_NONE) push(a);
}

/* A hat or stick direction on one axis: -1 / 0 / 1, held like a D-pad. */
static void axis_dir(int * cur, int now, ui_action_t neg, ui_action_t pos, uint32_t t)
{
    if(now == *cur) return;
    ui_action_t was = *cur < 0 ? neg : *cur > 0 ? pos : ACT_NONE;
    *cur = now;
    if(now != 0) hold_start(now < 0 ? neg : pos, t, false);
    else if(held_dir == was && !held_remote) held_dir = ACT_NONE;
}

static void extra_abs(extra_t * x, uint16_t code, int32_t v, uint32_t t)
{
    if(code == ABS_HAT0X) { axis_dir(&x->hat_x, v < 0 ? -1 : v > 0 ? 1 : 0, ACT_LEFT, ACT_RIGHT, t); return; }
    if(code == ABS_HAT0Y) { axis_dir(&x->hat_y, v < 0 ? -1 : v > 0 ? 1 : 0, ACT_UP, ACT_DOWN, t); return; }
    if(!x->has_stick || (code != ABS_X && code != ABS_Y)) return;
    int k = code == ABS_X ? 0 : 1;
    int mid = (x->ax_min[k] + x->ax_max[k]) / 2, half = (x->ax_max[k] - x->ax_min[k]) / 2;
    if(half <= 0) return;
    int pct = (int)((long)(v - mid) * 100 / half);
    int * cur = k == 0 ? &x->stick_x : &x->stick_y;
    int now = *cur;
    if(pct >= STICK_ON) now = 1; else if(pct <= -STICK_ON) now = -1;
    else if(pct > -STICK_OFF && pct < STICK_OFF) now = 0;
    if(k == 0) axis_dir(cur, now, ACT_LEFT, ACT_RIGHT, t);
    else       axis_dir(cur, now, ACT_UP, ACT_DOWN, t);
}

/* Read one extra device; false when it has gone. */
static bool drain_extra(extra_t * x, uint32_t t)
{
    struct input_event ev[16];
    for(;;) {
        ssize_t n = read(x->fd, ev, sizeof(ev));
        if(n < 0) return errno == EAGAIN || errno == EINTR;
        if(n == 0) return true;
        int count = (int)(n / sizeof(ev[0]));
        for(int i = 0; i < count; i++) {
            if(ev[i].type == EV_KEY) extra_key(ev[i].code, ev[i].value, t);
            else if(ev[i].type == EV_ABS) extra_abs(x, ev[i].code, ev[i].value, t);
        }
    }
}

static void drain(int fd, uint32_t t)
{
    if(fd < 0) return;
    struct input_event ev[16];
    for(;;) {
        ssize_t n = read(fd, ev, sizeof(ev));
        if(n <= 0) break;                       /* EAGAIN = nothing more */
        int count = (int)(n / sizeof(ev[0]));
        for(int i = 0; i < count; i++) {
            if(ev[i].type == EV_KEY) handle_key(ev[i].code, ev[i].value, t);
        }
    }
}

void input_poll(void)
{
    uint32_t t = now_ms();
    drain(joy_fd, t);
    drain(vol_fd, t);
    /* devices coming and going: rescan a moment after /dev/input changes */
    if(ino_fd >= 0) {
        char buf[1024];
        while(read(ino_fd, buf, sizeof(buf)) > 0) rescan_due = true;
    }
    for(int i = 0; i < nextra; ) {
        if(!drain_extra(&extras[i], t)) { close_extra(i); continue; }
        i++;
    }
    if(rescan_due) { rescan_due = false; scan_devices(); }

    if(held_dir == ACT_NONE) return;

    /* A remote that stopped talking (no repeat, no release): treat as let go. */
    if(held_remote && t - held_news_ms >= HOLD_TIMEOUT_MS) { held_dir = ACT_NONE; return; }

    /* Software auto-repeat for a held direction, faster the longer it's held. */
    uint32_t held_for = t - held_since_ms;
    uint32_t rate = held_for >= REPEAT_ACCEL_MS ? REPEAT_FAST_MS : REPEAT_RATE_MS;
    if(held_for >= REPEAT_DELAY_MS && t - last_repeat_ms >= rate) {
        push(held_dir);
        last_repeat_ms = t;
    }
}
