/*
 * SimpletonOS UI - the small on-unit keyboard (0.21a, 15 Oct 2026).
 *
 * For naming a mixtape or a collection (Ian, 15 Oct: "people who like
 * collecting and organizing things will want to take the time to name
 * their new mixtapes and collections, especially if they are going to be
 * seeing it in cool fonts on their TV"). A shared screen over whatever is
 * showing, in the theme's colours and fonts: the text being typed on top
 * with a caret, then rows of keys the D-pad walks (wrapping at the edges),
 * and a control row: Shift, the next alphabet, Space, Delete, Done.
 *
 *   A       types the key (on a control key: does it)
 *   B       cancels (nothing changes)
 *   X       deletes the last letter          Y     a space
 *   Start   done                              Select the next alphabet
 *
 * so the TV remote (arrows, OK, Back) can do everything from the keys on
 * screen, and the handheld has shortcuts. Alphabets: Latin, Latin accents,
 * Cyrillic, Greek, digits and punctuation; Shift gives capitals (one
 * letter, then it drops - press twice to lock). Up to KEYBOARD_MAX bytes.
 *
 * Why our own: the Linux on-screen keyboards (Onboard, Squeekboard,
 * Maliit, wvkbd, fbkeyboard) are X11 / Wayland / touch programs; the UI is
 * a DRM/KMS LVGL app driven by a D-pad. Layouts are tables in keyboard.c;
 * more alphabets are a table each (xkeyboard-config has them all), and
 * languages without one name things in the phone app (home-shelf-design-
 * notes.md). Chinese, Japanese and Korean need an input method: later.
 */
#ifndef SIMPLETON_KEYBOARD_H
#define SIMPLETON_KEYBOARD_H

#include "input.h"
#include <stdbool.h>

#define KEYBOARD_MAX 120

void keyboard_create(void);

/* Open with `title` over the text and `initial` in the field (the caret at
 * its end). `done` is called with the final text on Done (not on cancel);
 * an empty text counts as cancel. */
void keyboard_open(const char * title, const char * initial, void (*done)(const char * text, void * ctx), void * ctx);

bool keyboard_active(void);
void keyboard_handle_action(ui_action_t a);

#endif
