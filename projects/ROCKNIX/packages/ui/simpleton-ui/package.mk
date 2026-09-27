# SPDX-License-Identifier: GPL-2.0-or-later
# SimpletonOS package: the on-device UI. LVGL on DRM/KMS, no compositor.
#
# 0.2 (26 Sep 2026): folder browser driven by MPD's lsinfo, joypad input via
# evdev mapped to abstract actions, transport keys (play/pause, next/prev).
# Source layout (all in sources/, copied into ${PKG_BUILD} by scripts/unpack):
#   main.c     - display/theme/indev setup, action dispatch, main loop
#   input.c/h  - evdev joypad + volume rocker -> ui_action_t queue
#   mpdc.c/h   - minimal MPD text-protocol client (no libmpdclient)
#   browser.c/h- folder browser screen
#   theme.h    - shared style tokens
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

PKG_NAME="simpleton-ui"
PKG_VERSION="0.2"
PKG_LICENSE="GPL-2.0-or-later"
PKG_SITE="https://github.com/IamRoberty/distribution"
PKG_DEPENDS_TARGET="toolchain lvgl libdrm"
PKG_SECTION="graphics"
PKG_SHORTDESC="SimpletonOS on-device UI"
PKG_LONGDESC="LVGL + DRM/KMS user interface for SimpletonOS: MPD-backed folder browser with joypad navigation and transport control."
PKG_TOOLCHAIN="manual"

make_target() {
  ${CC} ${TARGET_CFLAGS} -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -DLV_KCONFIG_IGNORE \
    -DLV_LVGL_H_INCLUDE_SIMPLE \
    -I$(get_install_dir lvgl)/usr/include/lvgl \
    -I$(get_install_dir libdrm)/usr/include/libdrm \
    -o ${PKG_BUILD}/simpleton-ui \
    ${PKG_BUILD}/main.c ${PKG_BUILD}/input.c ${PKG_BUILD}/mpdc.c ${PKG_BUILD}/browser.c \
    ${TARGET_LDFLAGS} \
    -L$(get_install_dir lvgl)/usr/lib \
    -L$(get_install_dir libdrm)/usr/lib \
    -llvgl -ldrm -lm
}

makeinstall_target() {
  mkdir -p ${INSTALL}/usr/bin
  cp -a ${PKG_BUILD}/simpleton-ui ${INSTALL}/usr/bin
}

post_install() {
  enable_service simpleton-ui.service
}
