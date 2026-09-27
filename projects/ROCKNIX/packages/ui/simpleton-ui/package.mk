# SPDX-License-Identifier: GPL-2.0-or-later
# SimpletonOS package, 26 Sep 2026: the on-device UI itself. This first cut
# is a bring-up test, not the real UI - it opens the DRM display and draws a
# solid background plus one line of text, to prove the full chain
# (cross-compile -> image -> boot -> visible pixels on the panel) before any
# real screen logic is written. No MPD connection, no input handling yet.
#
# Structure follows this tree's own convention for first-party code with no
# upstream (modelled on kernel-drivers/device-tree-overlays): our source sits
# in sources/, which scripts/unpack copies into ${PKG_BUILD}, and the build
# steps are explicit make_target/makeinstall_target functions under
# PKG_TOOLCHAIN="manual". No PKG_URL, no CMake - one source file doesn't
# justify a build system.
#
# Linking notes:
#   - liblvgl.a is static, so the DRM driver's own dependency on libdrm has
#     to be satisfied here at final link time: LVGL's build references libdrm
#     nowhere, so -ldrm is our job. Order matters - -llvgl before -ldrm.
#   - -I.../include/lvgl makes both "lvgl.h" and the installed
#     "src/drivers/display/drm/lv_linux_drm.h" resolve, and lets LVGL's
#     __has_include check find the installed lv_conf.h automatically.
#   - LV_KCONFIG_IGNORE / LV_LVGL_H_INCLUDE_SIMPLE mirror the defines the
#     library itself was compiled with. Keep them in sync: lv_conf settings
#     determine struct layouts, so a consumer that sees different config than
#     the library was built with is an ABI mismatch, not a compile error.

PKG_NAME="simpleton-ui"
PKG_VERSION="0.1"
PKG_LICENSE="GPL-2.0-or-later"
PKG_SITE="https://github.com/IamRoberty/distribution"
PKG_DEPENDS_TARGET="toolchain lvgl libdrm"
PKG_SECTION="graphics"
PKG_SHORTDESC="SimpletonOS on-device UI (bring-up test)"
PKG_LONGDESC="Draws to the panel via LVGL + DRM/KMS. Current build is a bring-up test only: solid background and one line of text, no MPD connection or input handling yet."
PKG_TOOLCHAIN="manual"

make_target() {
  ${CC} ${TARGET_CFLAGS} \
    -DLV_KCONFIG_IGNORE \
    -DLV_LVGL_H_INCLUDE_SIMPLE \
    -I$(get_install_dir lvgl)/usr/include/lvgl \
    -I$(get_install_dir libdrm)/usr/include/libdrm \
    -o ${PKG_BUILD}/simpleton-ui \
    ${PKG_BUILD}/main.c \
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
