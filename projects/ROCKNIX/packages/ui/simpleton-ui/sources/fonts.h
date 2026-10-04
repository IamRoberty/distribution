/*
 * SimpletonOS UI - text fonts for every script (30 Sep 2026).
 *
 * Until 0.8 all on-screen text came from LVGL's built-in Montserrat bitmaps,
 * which hold plain ASCII and little else: "Beyoncé" showed a box, and every
 * Cyrillic, Vietnamese, CJK, Thai, Arabic or Hebrew tag was boxes end to
 * end. In the markets SimpletonOS is aimed at, that is most of the library.
 *
 * Now text is rendered through FreeType from TrueType/OpenType files at the
 * exact pixel size the layout asks for, and every font is the head of a
 * FALLBACK CHAIN: a letter the theme's typeface lacks is looked up in Noto
 * Sans (Latin, Vietnamese, Cyrillic, Greek), then Noto Sans CJK (Chinese,
 * Japanese, Korean), Arabic/Farsi/Urdu, the Indic scripts (Devanagari,
 * Bengali, Tamil, Telugu, Gujarati, Kannada, Malayalam, Gurmukhi, Odia,
 * Sinhala), Thai, Burmese, Khmer, Lao, Hebrew, Georgian, Armenian and
 * Ethiopic, and last LVGL's Montserrat for the LV_SYMBOL_* glyphs.
 * The theme chooses the typeface for the UI (a "uifont <file.ttf>" line in
 * its .theme); the Noto chain behind it is always the same, so a theme is
 * free to pick any Latin display face without losing a single script.
 *
 * Not handled yet (needs a shaping engine, planned as its own step): Indic,
 * Burmese, Khmer and Sinhala are drawn letter by letter without conjuncts or
 * reordering, so they are identifiable but not correctly formed; Kurdish
 * letters with no Unicode presentation forms (ڕ ڵ ێ) don't join.
 *
 * Fonts are created lazily per size on the UI thread and never freed: the
 * UI uses a handful of sizes and lives as long as the process.
 */
#ifndef SIMPLETON_FONTS_H
#define SIMPLETON_FONTS_H

#include <stdbool.h>

/* Start FreeType and read the active theme's "uifont" line. Call once,
 * after lv_init() and before any screen is built. Returns false when
 * FreeType is unavailable; ui_font() then returns the built-in Montserrat
 * sizes and the UI runs exactly as before. */
bool fonts_init(void);

#endif
