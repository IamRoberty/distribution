/*
 * SimpletonOS UI - string table (0.11, 4 Oct 2026).
 *
 * Every word the UI shows of its own (menus, messages, hints) is a short key
 * in strings.def, and each language is one plain text file:
 *
 *   <share>/lang/<code>.txt        (/usr/share/simpleton, or $SIMPLETON_SHARE)
 *
 *     # comment
 *     browser.empty = Nothing here
 *     track.numbered = Track {n}
 *
 * Adding a language = adding a file. Which one: $SIMPLETON_LANG when set
 * (for testing), else the first line of /storage/.config/simpleton/language
 * (the Settings page will write it), else "en". A key the file lacks falls
 * back to the built-in English, so a half-finished translation still works.
 *
 * Library text (artist, album, track titles) never comes through here: it is
 * shown as tagged, and only needs fonts (fonts.h).
 *
 * No LVGL in here, and no allocation after strings_init(): mpdc.c uses it,
 * and that file must stay able to move into a daemon.
 */
#ifndef SIMPLETON_STRINGS_H
#define SIMPLETON_STRINGS_H

#include <stddef.h>
#include <stdio.h>

typedef enum {
#define STR(id, key, en) S_##id,
#include "strings.def"
#undef STR
    S_COUNT
} str_id_t;

/* Load the language file. Call once at start-up, before any screen is
 * built; safe to skip (everything then reads as built-in English). */
void strings_init(void);

/* The text for a key, in the active language. Never NULL. */
const char * T(str_id_t id);

/* T(id) with its {name} places filled in. Arguments are name/value pairs of
 * strings ending in NULL:
 *     ui_strf(buf, sizeof(buf), S_TRACK_NUMBERED, "n", "03", NULL);
 * A {name} with no matching argument is left as written. Always terminates
 * `out`; returns `out`. */
char * ui_strf(char * out, size_t len, str_id_t id, ...);

/* The active language code ("en"). */
const char * strings_language(void);

/* Write the built-in English table in language-file form (--dump-lang):
 * the master copy of en.txt and the template for a new language. */
void strings_dump(FILE * f);

#endif
