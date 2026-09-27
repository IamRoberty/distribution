/*
 * SimpletonOS UI - input abstraction.
 *
 * The UI never sees button names. It sees actions. The joypad is the first
 * producer of those actions; a Bluetooth remote or an HDMI-CEC translator is
 * just another producer feeding the same queue, and touches no UI code.
 *
 * Button grammar (settled): A = select (hold: object menu), B = back,
 * Y = play/pause, X = skip (hold: previous). D-pad navigates with software
 * auto-repeat, since the kernel driver ships with autorepeat disabled.
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

#endif
