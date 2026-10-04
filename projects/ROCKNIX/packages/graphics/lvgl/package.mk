# SPDX-License-Identifier: GPL-2.0-or-later
# SimpletonOS package, 26 Sep 2026: LVGL, cross-compiled as a static library
# for the on-device UI (simpleton-ui). Built with a Simpleton-supplied
# lv_conf.h (config/lv_conf.h next to this file) rather than Kconfig, which
# keeps the dependency list small - no Python/pcpp needed.
#
# 30 Sep 2026: LV_USE_FREETYPE, LV_USE_BIDI and LV_USE_ARABIC_PERSIAN_CHARS
# are on in lv_conf.h for multi-script text (see simpleton-ui/sources/fonts.h).
#
# Only LV_USE_LINUX_DRM is enabled in that config. It's the dumb-buffer KMS
# path (LV_USE_LINUX_DRM_GBM_BUFFERS stays 0), so this does NOT need libgbm,
# EGL or GLES. Demos, examples and the bundled ThorVG vector-graphics lib are
# all switched off; none are used yet and ThorVG in particular is a sizeable
# C++ dependency not worth carrying for a static background/text bring-up.
#
# GOTCHA (found the hard way, 26 Sep 2026): LVGL's CMake does not reference
# libdrm at all, even with LV_USE_LINUX_DRM enabled - it neither links it nor
# puts its headers on the include path. Two consequences:
#
#   1. Compiling lv_linux_drm.c needs libdrm's headers here. libdrm splits
#      them: xf86drm.h / xf86drmMode.h land in /usr/include, but the
#      kernel-derived ones (drm_fourcc.h, drm.h, drm_mode.h) land in
#      /usr/include/libdrm - so that subdirectory has to be added explicitly
#      or the build dies on "drm_fourcc.h: No such file or directory".
#   2. Linking -ldrm is the consumer's job, handled in simpleton-ui's
#      package.mk. Nothing to do here, since a static library is just an
#      object archive and does no linking of its own.

PKG_NAME="lvgl"
PKG_VERSION="9.5.0"
PKG_SHA256="34a955cdf3a2d005507b704e87357af669a114523b6d3f77b5344fdc68717bc6"
PKG_ARCH="any"
PKG_LICENSE="MIT"
PKG_SITE="https://lvgl.io"
PKG_URL="https://github.com/lvgl/lvgl/archive/refs/tags/v${PKG_VERSION}.tar.gz"
PKG_DEPENDS_TARGET="toolchain libdrm freetype"
PKG_SECTION="graphics"
PKG_SHORTDESC="LVGL: Light and Versatile Graphics Library"
PKG_LONGDESC="LVGL (${PKG_VERSION}), cross-compiled as a static library with LV_USE_LINUX_DRM enabled for the SimpletonOS UI."
PKG_TOOLCHAIN="cmake"

PKG_CMAKE_OPTS_TARGET="-DLV_BUILD_CONF_PATH=${PKG_DIR}/config/lv_conf.h \
                       -DBUILD_SHARED_LIBS=OFF \
                       -DCONFIG_LV_BUILD_DEMOS=OFF \
                       -DCONFIG_LV_BUILD_EXAMPLES=OFF \
                       -DCONFIG_LV_USE_THORVG_INTERNAL=OFF"

# See gotcha 1 above. Same pattern the mpd package uses for lame's headers.
# FreeType (LV_USE_FREETYPE, 30 Sep 2026): same story - LVGL's CMake finds
# neither the headers nor the library; headers here, -lfreetype in simpleton-ui.
TARGET_CFLAGS+=" -I$(get_install_dir libdrm)/usr/include/libdrm -I$(get_install_dir freetype)/usr/include/freetype2"
