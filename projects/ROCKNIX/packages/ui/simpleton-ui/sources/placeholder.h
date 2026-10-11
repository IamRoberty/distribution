/*
 * SimpletonOS UI - designed placeholder art for tracks with no cover.
 *
 * Instead of a blank tile, a track with no art gets a piece of designed
 * artwork (a cassette for now, vinyl and others later) tinted in the colours
 * of the active theme, with artist / album / title written on its label.
 *
 * Assets live under SHARE_DIR (/usr/share/simpleton, or $SIMPLETON_SHARE):
 *
 *   placeholder/<design>/base.png          grayscale master with all shading
 *   placeholder/<design>/mask-<zone>.png   white = this zone takes a colour
 *   placeholder/<design>/light.png         optional: white = light added on top
 *                                          after the colours (a sheen on a dark disc)
 *   placeholder/<design>/layout.txt        "size W H", then text lines: centre,
 *                                          baseline, cap height, max width; and/or
 *                                          "area <artist|album|title> x0 y0 x1 y1"
 *                                          rectangles making one shape per name,
 *                                          the text fitted into it (master pixels)
 *   fonts/<name>.ttf                       OFL fonts
 *   themes/<name>.theme                    fonts per line + colourways
 *
 * Which theme: first line of /storage/.config/simpleton/theme, else "pastel".
 * The colourway is picked at random per track, never the same twice running.
 *
 * Rendering is the same maths as the Krita mock-ups: each zone's colour is
 * multiplied onto the grayscale base through its mask, the text is drawn in
 * the colourway's ink colour, and the result is XRGB8888 at the art box size
 * so the now-playing screen shows it exactly like a real cover.
 *
 * Threading: placeholder_init() runs on the UI thread at start-up;
 * placeholder_render() runs on the art worker thread, and the album page
 * renders its square's picture on a thread of its own (0.17h): fonts and
 * designs load under one lock, a loaded one never changes.
 */
#ifndef SIMPLETON_PLACEHOLDER_H
#define SIMPLETON_PLACEHOLDER_H

#include <stdbool.h>
#include <stdint.h>

/* Load the theme, its design and fonts, scaled for a `box` x `box` output.
 * Returns false (with a log line) if anything is missing; callers then fall
 * back to the plain placeholder. */
bool placeholder_init(int box);

/* Render for one track (`uri`: its MPD path, for the album page's colours
 * below). Empty strings are allowed. Returns a malloc'd XRGB8888 buffer
 * (box * box * 4 bytes) or NULL. */
uint8_t * placeholder_render(const char * uri, const char * artist, const char * album, const char * title, int * w, int * h);

/* The theme's colourways and label fonts, for tiles drawn in the tape's
 * colours without the tape (the grid's no-art tiles, 0.16). A colourway's
 * `label` zone colour and its ink; a line's font file (1 = artist, 2 =
 * album, 3 = track) and whether it is set in capitals. */
int          placeholder_colourway_count(void);
bool         placeholder_colourway(int i, uint32_t * label_rgb, uint32_t * ink_rgb);
const char * placeholder_line_font(int line, bool * caps);

/* ---- 0.17h: designs by name, explicit colours, an album's own colours ----
 *
 * A design is a folder under <share>/placeholder/<name>/ (base.png, the
 * mask-<zone>.png files, layout.txt); the theme's "design" line names the
 * one now-playing uses for tracks without art, others ("record") are loaded
 * on first ask. Zones known: background, shell, label, disc, sleeve, centre,
 * groove, spindle. */
typedef struct { const char * zone; uint32_t rgb; } placeholder_tint_t;

bool         placeholder_has_design(const char * name);
const char * placeholder_main_design(void);
int          placeholder_design_lines(const char * name);   /* its text lines (0 for a record with nothing written on it) */

/* Render `design` (NULL = the theme's) in given colours: zones named in
 * `tints` take their colour, the rest `fallback`; text in `ink`, the album
 * line in `album_font` (NULL = the theme's line-2 font) set in caps when
 * asked. Caller frees. */
uint8_t * placeholder_render_design(const char * design, const placeholder_tint_t * tints, int ntints, uint32_t fallback,
                                    uint32_t ink, const char * album_font, bool album_caps,
                                    const char * artist, const char * album, const char * title, int * w, int * h);

/* Remember colours for one album: placeholder_render() for a track of it
 * (one of `uris`, or the same artist and album ignoring case) uses them
 * instead of a random colourway, so now-playing's cassette matches the
 * album's tile and page. Thread-safe (the art worker renders, the page
 * sets). A NULL artist clears. */
void placeholder_prefer(const char * const * uris, int nuris, const char * artist, const char * album,
                        const placeholder_tint_t * tints, int ntints, uint32_t fallback,
                        uint32_t ink, const char * album_font, bool album_caps);

#endif
