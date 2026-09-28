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
# Source layout (all in sources/, copied into ${PKG_BUILD} by scripts/unpack):
#   main.c        - display/theme/indev setup, screen switch, action dispatch
#   input.c/h     - evdev joypad + volume rocker -> ui_action_t queue
#   mpdc.c/h      - minimal MPD text-protocol client (no libmpdclient)
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

PKG_NAME="simpleton-ui"
PKG_VERSION="0.6"
PKG_LICENSE="GPL-2.0-or-later"
PKG_SITE="https://github.com/IamRoberty/distribution"
PKG_DEPENDS_TARGET="toolchain lvgl libdrm libjpeg-turbo libpng zlib"
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
    -o ${PKG_BUILD}/simpleton-ui \
    ${PKG_BUILD}/main.c ${PKG_BUILD}/input.c ${PKG_BUILD}/mpdc.c ${PKG_BUILD}/browser.c \
    ${PKG_BUILD}/nowplaying.c ${PKG_BUILD}/art.c ${PKG_BUILD}/placeholder.c \
    ${TARGET_LDFLAGS} \
    -L$(get_install_dir lvgl)/usr/lib \
    -L$(get_install_dir libdrm)/usr/lib \
    -L$(get_install_dir libjpeg-turbo)/usr/lib \
    -L$(get_install_dir libpng)/usr/lib \
    -L$(get_install_dir zlib)/usr/lib \
    -llvgl -ldrm -ljpeg -lpng16 -lz -lpthread -lm
}

makeinstall_target() {
  mkdir -p ${INSTALL}/usr/bin ${INSTALL}/usr/share/simpleton
  cp -a ${PKG_BUILD}/simpleton-ui ${INSTALL}/usr/bin
  cp -a ${PKG_BUILD}/share/. ${INSTALL}/usr/share/simpleton/
}

post_install() {
  enable_service simpleton-ui.service
}
