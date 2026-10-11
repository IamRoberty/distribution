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
#
# 11 Oct 2026: the unit reset itself playing DST-compressed SACD tracks -
# the plugin's DST decoder ran one thread per core and four A55s at 1.8 GHz
# took the RK3566 past its 95 C hardware thermal shutdown (no log, no
# panic; proven with a clock cap). Three pieces, all here:
#   - simpleton-thermal (+ its service, enabled below): a 1104 MHz clock
#     ceiling and passive trip points at 75/82 C from every boot;
#   - mpd.conf: dstdec_threads "2" (MPD patch 0005) and restore_paused "yes"
#     (a boot never resumes the track that caused a reset); the migrate
#     script adds both - and the stereo-only decoder blocks - to a unit's
#     existing mpd.conf when they are missing;
#   - the DAC hotplug moved to an instanced unit started by udev.d/96-...
#     (simpleton-dac-hotplug@.service; the old non-instanced file is gone),
#     and simpleton-dac-detect waits for the card and checks the restart.
#   MPD patch 0006 stops MPD aborting (status=6/ABRT) at every stop.
# scripts/install copies system.d/* and udev.d/* into the image by itself;
# only the scripts and the config are copied here, and enabling is below.
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
  cp ${PKG_DIR}/scripts/simpleton-thermal ${INSTALL}/usr/bin
  chmod 0755 ${INSTALL}/usr/bin/simpleton-thermal

  mkdir -p ${INSTALL}/usr/config/mpd
  cp ${PKG_DIR}/config/mpd.conf ${INSTALL}/usr/config/mpd/mpd.conf
}

post_install() {
  enable_service "mpd.service"
  enable_service "simpleton-thermal.service"
}
