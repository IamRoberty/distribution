# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2009-2012 Stephan Raue (stephan@openelec.tv)
# Copyright (C) 2019-present Team LibreELEC (https://libreelec.tv)
#
# Simpleton override, 31 Aug 2026: upstream's pinned PKG_SHA256 no longer
# matches what codeberg.org serves for this tag (verified independently -
# a real drift in the archive/TAG.tar.gz source, not a bad download).
# Worth reporting upstream to ROCKNIX; remove this override once fixed there.
# Everything else below is an exact copy of the original.

PKG_NAME="libid3tag"
PKG_VERSION="0.16.4"
PKG_SHA256="2e9058af51e5f3881c13c55a9790abb9870812cc0f5917b6f3e825c6ae9b9f39"
PKG_LICENSE="GPL-2.0-or-later"
PKG_SITE="https://www.underbit.com/products/mad/"
PKG_URL="https://codeberg.org/tenacityteam/libid3tag/archive/${PKG_VERSION}.tar.gz"
PKG_DEPENDS_TARGET="toolchain zlib"
PKG_LONGDESC="A library for id3 tagging."

PKG_CMAKE_OPTS_TARGET="-DBUILD_SHARED_LIBS=OFF"
