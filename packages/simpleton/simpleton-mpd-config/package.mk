# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 Simpleton

# MPD upstream (packages/addons/service/mpd) is packaged as a Kodi-style
# add-on (PKG_IS_ADDON="yes"): its own install path only produces a
# downloadable add-on zip, not something baked into the base image.
# We still get its normal build/staging output for free (PKG_INSTALL is
# populated the same way regardless of PKG_IS_ADDON), so this package just
# depends on it and copies the already-built binaries into the base image
# ourselves - the same thing mpd's own addon() function does, just aimed
# at ${INSTALL} instead of ${ADDON_BUILD}. Zero edits to mpd's package.mk.

PKG_NAME="simpleton-mpd-config"
PKG_VERSION=""
PKG_LICENSE="GPL"
PKG_SITE=""
PKG_URL=""
PKG_DEPENDS_TARGET="toolchain mpd mpd-mpc libmpdclient simpleton-media-mount"
PKG_LONGDESC="Simpleton: MPD promoted to a core, always-on service with SimpletonOS config."
# 4 Oct 2026: per-card databases. mpd.conf gets a database block with a
# cache_directory, simpleton-mpd-conf-migrate converts older units' files,
# and mpd.service calls simpleton-card remount-all (simpleton-media-mount)
# after each start. The MPD side is patch 0004 in projects/ROCKNIX/patches/mpd.
PKG_TOOLCHAIN="manual"

makeinstall_target() {
  mkdir -p ${INSTALL}/usr/bin
  cp -P $(get_install_dir mpd)/usr/bin/mpd ${INSTALL}/usr/bin
  cp -P $(get_install_dir mpd-mpc)/usr/bin/mpc ${INSTALL}/usr/bin

  mkdir -p ${INSTALL}/usr/lib.private
  cp -p $(get_install_dir libmpdclient)/usr/lib/libmpdclient.so ${INSTALL}/usr/lib.private
  cp -p $(get_install_dir libmpdclient)/usr/lib/libmpdclient.so.2 ${INSTALL}/usr/lib.private

  patchelf --add-rpath '${ORIGIN}/../lib.private' ${INSTALL}/usr/bin/mpd
  patchelf --add-rpath '${ORIGIN}/../lib.private' ${INSTALL}/usr/bin/mpc

  cp ${PKG_DIR}/scripts/simpleton-dac-detect ${INSTALL}/usr/bin
  chmod 0755 ${INSTALL}/usr/bin/simpleton-dac-detect
  cp ${PKG_DIR}/scripts/simpleton-mpd-conf-migrate ${INSTALL}/usr/bin
  chmod 0755 ${INSTALL}/usr/bin/simpleton-mpd-conf-migrate

  mkdir -p ${INSTALL}/usr/config/mpd
  cp ${PKG_DIR}/config/mpd.conf ${INSTALL}/usr/config/mpd/mpd.conf
}

post_install() {
  enable_service "mpd.service"
}
