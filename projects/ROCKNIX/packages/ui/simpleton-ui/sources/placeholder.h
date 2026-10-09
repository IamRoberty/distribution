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
 *   placeholder/<design>/layout.txt        text lines: centre, baseline, cap
 *                                          height, max width (master pixels)
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
 * placeholder_render() runs on the art worker thread.
 */
#ifndef SIMPLETON_PLACEHOLDER_H
#define SIMPLETON_PLACEHOLDER_H

#include <stdbool.h>
#include <stdint.h>

/* Load the theme, its design and fonts, scaled for a `box` x `box` output.
 * Returns false (with a log line) if anything is missing; callers then fall
 * back to the plain placeholder. */
bool placeholder_init(int box);

/* Render for one track. Empty strings are allowed. Returns a malloc'd
 * XRGB8888 buffer (box * box * 4 bytes) or NULL. */
uint8_t * placeholder_render(const char * artist, const char * album, const char * title, int * w, int * h);

/* The theme's colourways and label fonts, for tiles drawn in the tape's
 * colours without the tape (the grid's no-art tiles, 0.16). A colourway's
 * `label` zone colour and its ink; a line's font file (1 = artist, 2 =
 * album, 3 = track) and whether it is set in capitals. */
int          placeholder_colourway_count(void);
bool         placeholder_colourway(int i, uint32_t * label_rgb, uint32_t * ink_rgb);
const char * placeholder_line_font(int line, bool * caps);

#endif
