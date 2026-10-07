/*
 * SimpletonOS UI - the settings files (0.13, 7 Oct 2026).
 *
 * Every setting is one small plain-text file under
 *
 *   /storage/.config/simpleton/<name>          ($SIMPLETON_CONFIG overrides, for tests)
 *
 * holding its value on the first line ("1", "off", "archer", "L"). The card
 * scripts and the cache tool read the same files with a shell `cat`, so
 * nothing cleverer than that is allowed here: no sections, no quoting.
 * Writes go through a temporary name and a rename, so a power cut mid-write
 * leaves the old value, never half a file.
 *
 * No LVGL in here: playthrough.c and the cache tool use it too.
 */
#ifndef SIMPLETON_CONFIG_H
#define SIMPLETON_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#define CONFIG_DIR_DEFAULT "/storage/.config/simpleton"

/* The settings folder in use. */
const char * config_dir(void);

/* Full path of a setting's file. */
void config_path(const char * name, char * out, size_t len);

/* First line of the setting, trimmed, or `fallback` when the file is
 * missing or empty. Always terminates `out`; returns `out`. */
char * config_read(const char * name, char * out, size_t len, const char * fallback);

/* "1", "on", "yes", "true" (case-insensitive) are true, anything else false;
 * a missing file gives `fallback`. */
bool config_read_bool(const char * name, bool fallback);

/* Write the value (one line). Creates the folder. False on any failure. */
bool config_write(const char * name, const char * value);

/* ---- the active theme ----
 *
 * One word in the `theme` setting, default THEME_DEFAULT. The theme's own
 * file is <share>/themes/<name>.theme (plain text, one setting per line;
 * placeholder.c, fonts.c and home.c each read the lines they own), and its
 * art sits beside it in <share>/themes/<name>/. <share> is
 * /usr/share/simpleton, or $SIMPLETON_SHARE. */
#define SHARE_DIR_DEFAULT  "/usr/share/simpleton"
#define THEME_DEFAULT      "archer"

const char * config_share_dir(void);
const char * config_theme_name(void);                                  /* "archer"                */
void config_theme_file(char * out, size_t len);                        /* .../themes/<name>.theme */
void config_theme_asset(const char * file, char * out, size_t len);    /* .../themes/<name>/<file>; absolute paths pass through */

#endif
