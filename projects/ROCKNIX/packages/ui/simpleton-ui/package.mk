# SPDX-License-Identifier: GPL-2.0-or-later
# SimpletonOS package: the on-device UI. LVGL on DRM/KMS, no compositor.
#
# 0.2 (26 Sep 2026): folder browser driven by MPD's lsinfo, joypad input via
# evdev mapped to abstract actions, transport keys (play/pause, next/prev).
# 0.3 (27 Sep 2026): now-playing screen with full-bleed album art, decoded on
# a worker thread by libjpeg-turbo / libpng; format line reads the DAC's real
# state from /proc/asound.
# 0.4 (28 Sep 2026): designed placeholder art for tracks without a cover -
# themed cassette tinted per colourway, tags written on the label. Assets
# (layers, fonts, themes) ship from sources/share to /usr/share/simpleton.
# 0.5 (28 Sep 2026): art paging - with the now-playing controls hidden,
# D-pad left/right pages through every image in the album folder and its
# Artwork/Scans subfolders; "2 / 11" badge top-right.
# 0.6 (28 Sep 2026): wide pages (gatefolds, booklet spreads) fill the screen
# height and left/right pans across them a quarter screen per press before
# turning the page.
# 0.7 (29 Sep 2026): HDMI. Own DRM backend (display.c) instead of LVGL's
# driver: HDMI when connected else the panel, max 1080p on a TV, the unused
# screen switched off; hotplug restarts the UI on the new output keeping
# the screen and folder. Layouts sit on a square stage sized to the screen's
# short edge (PX()/ui_font() in theme.h); on a TV the now-playing sides take
# a muted colour from the art. Needs LV_FONT_MONTSERRAT_30/42/48 in lv_conf.h.
# 0.8 (30 Sep 2026): HDMI-CEC. The TV remote drives the UI through /dev/cec0
# (cec.c, kernel CEC API, no libcec). Appears as Playback device "Simpleton";
# never wakes or sleeps the TV; TV standby never stops playback.
# 0.9 (30 Sep 2026): text in every script. FreeType fonts at the exact scaled
# size with a fallback chain (theme typeface -> Noto Sans -> Noto CJK -> Thai,
# Arabic, Hebrew, Devanagari, Tamil, Bengali -> Montserrat for LV_SYMBOL_*),
# BIDI + Arabic shaping on in lv_conf.h. Noto files ship in share/fonts (OFL);
# CJK comes from ROCKNIX's noto-sans-cjk package. The cassette label falls
# back to Noto Sans too. Indic scripts render unshaped until a shaping step.
# 0.10 (1 Oct 2026): living-room pass.
#   - Transparent containers: a SACD ISO, a multi-track DFF (the SACD patch
#     makes every .dff a container) or a CUE album lists as its tracks in
#     place, like any album; two discs in one folder simply run on.
#     Done in mpdc.c (mpd_lsinfo_expanded) so every client sees the same.
#   - Play through folders (playthrough.c): HiBy's walk, on by default,
#     /storage/.config/simpleton/play_through = 0 turns it off. The next
#     folder is queued as the last track starts, so it stays gapless.
#   - Lists: left/right page a screenful; one accelerating auto-repeat for
#     the D-pad and the TV remote (CEC only says held/released now), with
#     a safety stop when a TV never sends "released".
# 0.11 (4 Oct 2026): groundwork for the expanded UI (Engineering Note 05,
# build steps 1 and 2). Nothing looks different on screen.
#   - String table (strings.c, strings.def): every word the UI shows of its
#     own is a short key; each language is one plain text file in
#     share/lang (en.txt to start), chosen by
#     /storage/.config/simpleton/language. English is also built in, so a
#     missing file never blanks the screen. No hard-coded text after this.
#   - Screen-size-aware layout (layout.c): the real screen, handheld or TV,
#     and the sizes XXL..S as row counts; grid and list measurements are
#     worked out from those (handheld 1x1..4x4, TV 1x2..5x10). The browser
#     takes its measurements from it; nothing is tied to 720 px.
# 0.12 (4 Oct 2026): the library layer (Note 05 steps 3b and 3c). Nothing
# on screen changes yet; the screens that use it come next.
#   - library.c: the tag layer. One listallinfo per card, grouped into
#     albums (album artist / artist + title; a folder is an album when
#     there is no album tag; compilations marked), artists, the picker's
#     sorts with untagged items last, and folder types (album, collection,
#     artist, library, plain). Saved as library.idx.
#   - cache.c + simpleton-cache (a second binary from the same sources, no
#     LVGL): cover thumbnails at 192/256/384/512 px as JPEG, a colour per
#     album, kept under /storage/.simpleton/cache/<card>/ and mirrored to
#     the card's .simpleton/cache so it travels with the card. Only albums
#     whose tracks changed are redone. Runs at idle priority from
#     simpleton-cache@<card>.service, started by simpleton-card after a scan.
#   - art.c: MUSIC_DIR can be overridden for tests; synchronous cover fetch
#     and decode for the cache. mpdc.c: streaming listallinfo.
# 0.16f (9 Oct 2026): more colour in the grid's no-art tiles. Ian's twelve
# colour sets go in the theme as tilepalette lines and a tilecolours line
# picks how they're used (theme / palettes / mix / generate; tilestyle.c),
# dealt per visit so a screenful doesn't repeat and neighbours differ.
# Fonts unchanged pending a design talk.
# 0.16g (9 Oct 2026): inks after Ian's first look - mostly palette colours,
# a cream (FAEFD0) common but not dominant, some off-black; the contrast
# floor is a theme setting (Archer: 2.0, vibe over contrast). tilecontrast,
# tilecream and tileinks lines in the theme.
# Source layout (all in sources/, copied into ${PKG_BUILD} by scripts/unpack):
#   main.c        - theme/indev setup, screen switch, action dispatch
#   display.c/h   - DRM/KMS output choice, modeset, page flips, hotplug
#   theme.c       - the square stage behind theme.h
#   layout.c/h    - real screen, sizes XXL..S, grid and list measurements
#   strings.c/h   - string table; strings.def lists every key with its English
#   library.c/h   - the library index: albums, artists, sorts, folder types
#   cache.c/h     - thumbnail + index cache for a card, mirrored to the card
#   simpleton-cache.c - the cache tool's main (build / status / albums / folders)
#   fonts.c/h     - FreeType fonts with a Noto fallback chain per size
#   input.c/h     - evdev joypad + volume rocker -> ui_action_t queue, auto-repeat
#   cec.c/h       - HDMI-CEC: TV remote keys -> same ui_action_t queue
#   mpdc.c/h      - minimal MPD text-protocol client (no libmpdclient)
#   playthrough.c/h - play through folders, driven by MPD notices
#   browser.c/h   - folder browser screen
#   nowplaying.c/h- now-playing screen
#   art.c/h       - album art lookup + decode, worker thread
#   placeholder.c/h - themed placeholder art (tint + label text, stb_truetype)
#   stb_truetype.h- vendored, public domain (github.com/nothings/stb)
#   theme.h       - shared style tokens
#   share/        - placeholder layers, OFL fonts, theme files, lang/<code>.txt
#
# Structure follows kernel-drivers/device-tree-overlays: PKG_TOOLCHAIN="manual"
# with explicit make_target/makeinstall_target. No CMake for a handful of files.
#
# Linking notes:
#   - liblvgl.a is static, so libdrm is linked here (-llvgl before -ldrm).
#   - -I.../include/lvgl resolves "lvgl.h", the installed DRM driver header,
#     and LVGL's own __has_include of the installed lv_conf.h.
#   - LV_KCONFIG_IGNORE / LV_LVGL_H_INCLUDE_SIMPLE mirror the library build.
#     lv_conf settings determine struct layouts; a mismatch is an ABI bug,
#     not a compile error.
#   - libjpeg-turbo and libpng are ROCKNIX's own packages (shared libs, so
#     listing them in PKG_DEPENDS_TARGET is what puts the .so in the image).
#     libpng pulls zlib; -lz is linked explicitly since we link it by hand.
#   - freetype likewise (shared, already in the image for other packages);
#     liblvgl.a's FreeType code resolves against it at this link, and lvgl.h
#     pulls ft2build.h, hence the freetype2 include path here as well.

PKG_NAME="simpleton-ui"
PKG_VERSION="0.16g"
PKG_LICENSE="GPL-2.0-or-later"
PKG_SITE="https://github.com/IamRoberty/distribution"
PKG_DEPENDS_TARGET="toolchain lvgl libdrm libjpeg-turbo libpng zlib freetype noto-sans-cjk"
PKG_SECTION="graphics"
PKG_SHORTDESC="SimpletonOS on-device UI"
PKG_LONGDESC="LVGL + DRM/KMS user interface for SimpletonOS: MPD-backed folder browser and now-playing screen with album art, joypad navigation and transport control."
PKG_TOOLCHAIN="manual"

# liblvgl.a is linked statically, so a change to lvgl (e.g. lv_conf.h) must
# rebuild this package too; without this line the old LVGL stays baked in.
PKG_NEED_UNPACK="$(get_pkg_directory lvgl)"

make_target() {
  # -g and -rdynamic: the crash handler in main.c prints a backtrace; the
  # image build strips the installed copy, the one under build/ keeps the
  # symbols for addr2line (album-grid-0.16.md, 0.16c).
  ${CC} ${TARGET_CFLAGS} -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -DLV_KCONFIG_IGNORE \
    -DLV_LVGL_H_INCLUDE_SIMPLE \
    -DSIMPLETON_VERSION=\"${PKG_VERSION}\" \
    -I$(get_install_dir lvgl)/usr/include/lvgl \
    -I$(get_install_dir libdrm)/usr/include/libdrm \
    -I$(get_install_dir libjpeg-turbo)/usr/include \
    -I$(get_install_dir libpng)/usr/include \
    -I$(get_install_dir freetype)/usr/include/freetype2 \
    -o ${PKG_BUILD}/simpleton-ui \
    ${PKG_BUILD}/main.c ${PKG_BUILD}/input.c ${PKG_BUILD}/mpdc.c ${PKG_BUILD}/browser.c \
    ${PKG_BUILD}/nowplaying.c ${PKG_BUILD}/art.c ${PKG_BUILD}/placeholder.c \
    ${PKG_BUILD}/display.c ${PKG_BUILD}/theme.c ${PKG_BUILD}/cec.c ${PKG_BUILD}/fonts.c \
    ${PKG_BUILD}/playthrough.c ${PKG_BUILD}/strings.c ${PKG_BUILD}/layout.c \
    ${PKG_BUILD}/library.c ${PKG_BUILD}/config.c ${PKG_BUILD}/shelf.c ${PKG_BUILD}/home.c \
    ${PKG_BUILD}/settings.c ${PKG_BUILD}/notice.c ${PKG_BUILD}/picker.c \
    ${PKG_BUILD}/grid.c ${PKG_BUILD}/thumbs.c ${PKG_BUILD}/cache.c ${PKG_BUILD}/tilestyle.c \
    ${TARGET_LDFLAGS} -rdynamic \
    -L$(get_install_dir lvgl)/usr/lib \
    -L$(get_install_dir libdrm)/usr/lib \
    -L$(get_install_dir libjpeg-turbo)/usr/lib \
    -L$(get_install_dir libpng)/usr/lib \
    -L$(get_install_dir zlib)/usr/lib \
    -L$(get_install_dir freetype)/usr/lib \
    -llvgl -lfreetype -ldrm -ljpeg -lpng16 -lz -lpthread -lm

  # The cache tool: the same sources without LVGL or the display.
  ${CC} ${TARGET_CFLAGS} -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -I$(get_install_dir libjpeg-turbo)/usr/include \
    -I$(get_install_dir libpng)/usr/include \
    -o ${PKG_BUILD}/simpleton-cache \
    ${PKG_BUILD}/simpleton-cache.c ${PKG_BUILD}/cache.c ${PKG_BUILD}/library.c \
    ${PKG_BUILD}/art.c ${PKG_BUILD}/placeholder.c ${PKG_BUILD}/mpdc.c ${PKG_BUILD}/strings.c \
    ${PKG_BUILD}/config.c \
    ${TARGET_LDFLAGS} \
    -L$(get_install_dir libjpeg-turbo)/usr/lib \
    -L$(get_install_dir libpng)/usr/lib \
    -L$(get_install_dir zlib)/usr/lib \
    -ljpeg -lpng16 -lz -lpthread -lm
}

makeinstall_target() {
  mkdir -p ${INSTALL}/usr/bin ${INSTALL}/usr/share/simpleton
  cp -a ${PKG_BUILD}/simpleton-ui ${INSTALL}/usr/bin
  cp -a ${PKG_BUILD}/simpleton-cache ${INSTALL}/usr/bin
  cp -a ${PKG_BUILD}/share/. ${INSTALL}/usr/share/simpleton/
}

post_install() {
  enable_service simpleton-ui.service
}
