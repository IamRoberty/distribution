/*
 * SimpletonOS UI - a notice screen (0.13, 7 Oct 2026).
 *
 * A shared screen with a title and a line or two of text, for the places
 * the Home page lists before their view exists ("Albums - not built yet").
 * Back or Select returns to the screen it came from. Theme colours and
 * fonts only, like every shared screen.
 */
#ifndef SIMPLETON_NOTICE_H
#define SIMPLETON_NOTICE_H

#include "input.h"
#include <stdbool.h>

void notice_create(void);
void notice_show(const char * title, const char * text);
void notice_hide(void);

/* True when the user is done with it (Back / Select); main.c then returns. */
bool notice_handle_action(ui_action_t a);

#endif
