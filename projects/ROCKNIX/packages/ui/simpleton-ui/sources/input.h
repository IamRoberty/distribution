/*
 * SimpletonOS UI - input abstraction.
 *
 * The UI never sees button names. It sees actions. The joypad is the first
 * producer of those actions; a Bluetooth remote or an HDMI-CEC translator is
 * just another producer feeding the same queue, and touches no UI code.
 *
 * Button grammar (settled): A = select (hold: object menu), B = back,
 * Y = play/pause, X = skip (hold: previous), Start = now-playing screen,
 * Select = settings (0.13: the page of the view on screen).
 * D-pad navigates with software auto-repeat, since the kernel driver ships
 * with autorepeat disabled. The repeat speeds up the longer a direction is
 * held (1 Oct 2026), and other producers (CEC) use the same repeater through
 * input_hold_*(), so a held key feels identical from every control.
 */
#ifndef SIMPLETON_INPUT_H
#define SIMPLETON_INPUT_H

#include <stdbool.h>

typedef enum {
    ACT_NONE = 0,
    ACT_UP,
    ACT_DOWN,
    ACT_LEFT,
    ACT_RIGHT,
    ACT_SELECT,      /* A, short press          */
    ACT_MENU,        /* A, long press           */
    ACT_BACK,        /* B                       */
    ACT_PLAYPAUSE,   /* Y                       */
    ACT_NEXT,        /* X, short press          */
    ACT_PREV,        /* X, long press           */
    ACT_VOL_UP,      /* volume rocker           */
    ACT_VOL_DOWN,
    ACT_HOME,        /* Start: jump to now-playing */
    ACT_PLAY,        /* explicit play (remote keys; no joypad button) */
    ACT_PAUSE,       /* explicit pause (remote Pause / Stop)          */
    ACT_SETTINGS,    /* Select (314): this view's settings page (0.13) */
    ACT_HOMEPAGE,    /* F (316, BTN_MODE): the Home page (0.21a, Ian: the favourite button isn't needed, hold A does it) */
    ACT_TEXT,        /* a keyboard typed text while text entry is on: input_take_text() (0.26) */
    ACT_ERASE,       /* a keyboard's Backspace while text entry is on (0.26) */
} ui_action_t;

/* Open the joypad by its stable by-path name (never by eventN, which moves
 * when a USB DAC with its own buttons enumerates first). Returns false if it
 * can't be opened; the UI keeps running without input so the display still
 * comes up for diagnosis.
 *
 * 0.26: every other input device with keys is opened too - USB and
 * Bluetooth game pads, keyboards, media remotes - and /dev/input is watched
 * so devices that come and go (a Bluetooth pad connecting, a USB keyboard
 * plugged in) are picked up and dropped by themselves. Devices without keys
 * (a DAC's status interface) are left alone; the power key belongs to
 * power.c and adc-keys repeats the F button, so both stay closed here. */
bool input_init(void);

/* Call once per main-loop iteration. Drains pending evdev events and emits
 * timed actions (holds, repeats). Never blocks. */
void input_poll(void);

/* Pop the next action, or ACT_NONE when the queue is empty. */
ui_action_t input_next_action(void);

/* File descriptor for poll(): one descriptor that wakes for every input
 * device open and for devices coming and going (0.26), or -1. */
int input_fd(void);

/* Text entry (0.26): while on, a keyboard's printable keys queue text
 * (ACT_TEXT, read with input_take_text) and Backspace is ACT_ERASE instead
 * of Back; Enter is ACT_HOME (done) and Esc ACT_BACK either way. The
 * on-screen keyboard turns it on while it is up. */
void input_text_mode(bool on);
/* Re-read the input settings (swap_ab: A and B change places on every pad,
 * the built-in one included - 0.26a). */
void input_reload_settings(void);
/* The printed letters that mean select and back on the built-in pad, for
 * the hint lines: "A" / "B", or the other way round with Swap A and B. */
const char * input_letter_select(void);
const char * input_letter_back(void);
/* The text typed since the last call (UTF-8, at most len - 1 bytes). */
void input_take_text(char * out, int len);

/* For other producers (cec.c): queue an action as if a button made it.
 * Drops silently when the queue is full, never blocks. */
void input_push(ui_action_t a);

/* For other producers whose keys can be held (CEC navigation keys): the
 * software repeater. begin() emits the action once and starts repeating it
 * on the joypad's schedule; refresh() is called for every further "still
 * pressed" message the producer gets; end() stops. A hold that is neither
 * refreshed nor ended for a while stops by itself, so a TV that never sends
 * "released" can't scroll forever. */
void input_hold_begin(ui_action_t dir);
void input_hold_refresh(ui_action_t dir);
void input_hold_end(void);

#endif
