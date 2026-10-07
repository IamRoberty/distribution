# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 Simpleton

PKG_NAME="simpleton-media-mount"
PKG_VERSION=""
PKG_LICENSE="GPL"
PKG_SITE=""
PKG_URL=""
PKG_DEPENDS_TARGET="toolchain udevil"
PKG_LONGDESC="Simpleton: auto-mount microSD cards in the second slot for music, via udevil."
PKG_TOOLCHAIN="manual"

# Installed automatically by scripts/install:
#   udev.d/96-simpleton-sdcard-mount.rules
#       -> /usr/lib/udev/rules.d/
#   system.d/rocknix-automount.service.d/simpleton-disable.conf
#       -> /usr/lib/systemd/system/rocknix-automount.service.d/
#   system.d/simpleton-card-add@.service, simpleton-card-remove@.service
#       -> /usr/lib/systemd/system/
# Installed below:
#   scripts/simpleton-card -> /usr/bin (the insert/remove handler)
#
# Why this exists:
#   ROCKNIX mounts SD cards itself (rocknix-automount), for ROM storage at
#   /storage/games-external, and overrides udevil's rule to handle USB
#   storage only. SimpletonOS has no games, and the emulator package that
#   carried ROCKNIX's hot-plug rule is stripped from our build, so a music
#   card inserted into the second slot was never mounted at all.
#
#   This package hands SD cards to udevil instead, which mounts each card at
#   /var/media/<device>-<model>_<serial> (or its volume label, if it has one).
#   The name includes the card's hardware serial, so the same card always
#   lands at the same path - which keeps MPD's database valid across
#   pull-and-reinsert.
#
# Scanning (decided 1 Oct 2026, replacing "never scan automatically"):
#   inserting a card starts an incremental MPD scan of that card by default;
#   /storage/.config/simpleton/autoscan = "off" turns that off (Settings will
#   own it). Manual scan stays available regardless. Removing a card never
#   scans, so the library keeps its entries and a reinsert is quick.
#   Removal detaches the mount lazily: a busy unmount used to fail and leave
#   a dead mount that stopped the card mounting again (30 Sep 2026).
#
# Per-card libraries (4 Oct 2026, Engineering Note 05 section 10): each card
#   is mounted into MPD as card-<serial> with its own database file, kept in
#   MPD's cache folder and copied to the card's hidden .simpleton folder, so
#   a card is browsable the moment it is in, on any unit. See the header of
#   scripts/simpleton-card and MPD patch 0004.

makeinstall_target() {
  mkdir -p ${INSTALL}/usr/bin
  cp ${PKG_DIR}/scripts/simpleton-card ${INSTALL}/usr/bin
  chmod 0755 ${INSTALL}/usr/bin/simpleton-card
}
