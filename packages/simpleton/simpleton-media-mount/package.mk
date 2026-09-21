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

# Everything this package ships is installed automatically by scripts/install:
#   udev.d/96-simpleton-sdcard-mount.rules
#       -> /usr/lib/udev/rules.d/
#   system.d/rocknix-automount.service.d/simpleton-disable.conf
#       -> /usr/lib/systemd/system/rocknix-automount.service.d/
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
#   pull-and-reinsert. Scanning is deliberately NOT triggered here; the user
#   decides when to scan.

makeinstall_target() {
  : # nothing beyond the auto-installed udev.d and system.d content
}
