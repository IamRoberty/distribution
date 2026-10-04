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
# Source layout (all in sources/, copied into ${PKG_BUILD} by scripts/unpack):
#   main.c        - theme/indev setup, screen switch, action dispatch
#   display.c/h   - DRM/KMS output choice, modeset, page flips, hotplug
#   theme.c       - stage scaling helpers behind theme.h
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
#   share/        - placeholder layers, OFL fonts, theme files
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
PKG_VERSION="0.10"
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
  ${CC} ${TARGET_CFLAGS} -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -DLV_KCONFIG_IGNORE \
    -DLV_LVGL_H_INCLUDE_SIMPLE \
    -I$(get_install_dir lvgl)/usr/include/lvgl \
    -I$(get_install_dir libdrm)/usr/include/libdrm \
    -I$(get_install_dir libjpeg-turbo)/usr/include \
    -I$(get_install_dir libpng)/usr/include \
    -I$(get_install_dir freetype)/usr/include/freetype2 \
    -o ${PKG_BUILD}/simpleton-ui \
    ${PKG_BUILD}/main.c ${PKG_BUILD}/input.c ${PKG_BUILD}/mpdc.c ${PKG_BUILD}/browser.c \
    ${PKG_BUILD}/nowplaying.c ${PKG_BUILD}/art.c ${PKG_BUILD}/placeholder.c \
    ${PKG_BUILD}/display.c ${PKG_BUILD}/theme.c ${PKG_BUILD}/cec.c ${PKG_BUILD}/fonts.c \
    ${PKG_BUILD}/playthrough.c \
    ${TARGET_LDFLAGS} \
    -L$(get_install_dir lvgl)/usr/lib \
    -L$(get_install_dir libdrm)/usr/lib \
    -L$(get_install_dir libjpeg-turbo)/usr/lib \
    -L$(get_install_dir libpng)/usr/lib \
    -L$(get_install_dir zlib)/usr/lib \
    -L$(get_install_dir freetype)/usr/lib \
    -llvgl -lfreetype -ldrm -ljpeg -lpng16 -lz -lpthread -lm
}

makeinstall_target() {
  mkdir -p ${INSTALL}/usr/bin ${INSTALL}/usr/share/simpleton
  cp -a ${PKG_BUILD}/simpleton-ui ${INSTALL}/usr/bin
  cp -a ${PKG_BUILD}/share/. ${INSTALL}/usr/share/simpleton/
}

post_install() {
  enable_service simpleton-ui.service
}
