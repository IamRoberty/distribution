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
 * drivers (adc-keys and the joypad's sw11). We don't open adc-keys and don't
 * map BTN_MODE at all yet, so it can't double-fire.
 */
#include "input.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define JOYPAD_PATH "/dev/input/by-path/platform-rocknix-singleadc-joypad-event-joystick"
#define VOLKEYS_PATH "/dev/input/by-path/platform-gpio-keys-vol-event"

/* Timing, ms */
#define HOLD_MS          450   /* press longer than this = "hold"           */
#define REPEAT_DELAY_MS  400   /* D-pad: first repeat after this            */
#define REPEAT_RATE_MS   90    /* D-pad: then every this                    */

#define QUEUE_LEN 32

static int joy_fd = -1;
static int vol_fd = -1;

static ui_action_t queue[QUEUE_LEN];
static int q_head, q_tail;

/* Per-button press timestamps for hold detection (A and X). */
static uint32_t a_down_ms, x_down_ms;
static bool a_down, x_down;

/* Currently held D-pad direction for software auto-repeat. */
static ui_action_t held_dir = ACT_NONE;
static uint32_t held_since_ms, last_repeat_ms;

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

ui_action_t input_next_action(void)
{
    if(q_head == q_tail) return ACT_NONE;
    ui_action_t a = queue[q_head];
    q_head = (q_head + 1) % QUEUE_LEN;
    return a;
}

int input_fd(void) { return joy_fd; }

static int open_nonblock(const char * path)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if(fd < 0) fprintf(stderr, "simpleton-ui: input: cannot open %s: %s\n", path, strerror(errno));
    return fd;
}

bool input_init(void)
{
    joy_fd = open_nonblock(JOYPAD_PATH);
    vol_fd = open_nonblock(VOLKEYS_PATH);
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

    ui_action_t dir = dpad_action(code);
    if(dir != ACT_NONE) {
        if(pressed) {
            push(dir);
            held_dir = dir;
            held_since_ms = t;
            last_repeat_ms = t;
        }
        else if(held_dir == dir) {
            held_dir = ACT_NONE;
        }
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
        default:
            /* shoulders, select, F: unmapped. Logged once per press so a
             * button that turns out to use a different code is easy to spot. */
            if(pressed) fprintf(stderr, "simpleton-ui: input: unmapped key code %u\n", code);
            break;
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

    /* Software auto-repeat for a held D-pad direction. */
    if(held_dir != ACT_NONE && t - held_since_ms >= REPEAT_DELAY_MS && t - last_repeat_ms >= REPEAT_RATE_MS) {
        push(held_dir);
        last_repeat_ms = t;
    }
}
