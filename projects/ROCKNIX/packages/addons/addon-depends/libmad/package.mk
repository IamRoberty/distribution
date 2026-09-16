# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2009-2014 Stephan Raue (stephan@openelec.tv)
# Copyright (C) 2016-present Team LibreELEC (https://libreelec.tv)
#
# Simpleton override, 31 Aug 2026: upstream's pinned PKG_SHA256 no longer
# matches what codeberg.org serves for this commit. Verified independently.
# Worth reporting upstream to ROCKNIX; remove this override once fixed there.

PKG_NAME="libmad"
PKG_VERSION="be34ec9fe47577e7f3d84cc9640d2a4696d478d6"
PKG_SHA256="37d007860f68163f27c07d9b69244ade7397e3f8cdb0b8ec4fd57872ac1bcfdb"
PKG_LICENSE="GPL-2.0-or-later"
PKG_SITE="http://www.mars.org/home/rob/proj/mpeg/"
PKG_URL="https://codeberg.org/tenacityteam/libmad/archive/${PKG_VERSION}.tar.gz"
PKG_DEPENDS_TARGET="toolchain"
PKG_LONGDESC="A high-quality MPEG audio decoder."

PKG_CMAKE_OPTS_TARGET="-DBUILD_SHARED_LIBS=OFF"
if [ "${TARGET_ARCH}" = "x86_64" ]; then
  PKG_CMAKE_OPTS_TARGET+=" -DOPTIMIZE=ACCURACY"
fi
