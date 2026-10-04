/*
 * SimpletonOS UI - input abstraction.
 *
 * The UI never sees button names. It sees actions. The joypad is the first
 * producer of those actions; a Bluetooth remote or an HDMI-CEC translator is
 * just another producer feeding the same queue, and touches no UI code.
 *
 * Button grammar (settled): A = select (hold: object menu), B = back,
 * Y = play/pause, X = skip (hold: previous), Start = now-playing screen.
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
} ui_action_t;

/* Open the joypad by its stable by-path name (never by eventN, which moves
 * when a USB DAC with its own buttons enumerates first). Returns false if it
 * can't be opened; the UI keeps running without input so the display still
 * comes up for diagnosis. */
bool input_init(void);

/* Call once per main-loop iteration. Drains pending evdev events and emits
 * timed actions (holds, repeats). Never blocks. */
void input_poll(void);

/* Pop the next action, or ACT_NONE when the queue is empty. */
ui_action_t input_next_action(void);

/* File descriptor for poll(), or -1 when no joypad is open. */
int input_fd(void);

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
