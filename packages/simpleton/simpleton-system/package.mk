# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 Simpleton

# simpleton-system: the appliance profile (image round 1, 28 Oct 2026).
# What the bench unit ran by hand since 22 Oct (image-round-plan.md,
# batches 1 and 2) made part of the image:
#   - services an audio transport has no use for are MASKED: this image's
#     systemd reads /storage/.config/system.d instead of /etc/systemd/system
#     (patch 0001), so the masks are /dev/null links that REPLACE the unit
#     files - projects/ROCKNIX/filesystem/usr/lib/systemd/system/<unit>,
#     which scripts/image copies over the packages last. Samba (smbd, nmbd, wsdd2), rpcbind, avahi, NetworkManager
#     and iwd (no Wi-Fi this version), network-online (it waited a minute
#     for the masked NetworkManager and held the UI back), ROCKNIX's
#     volume-fixup, batteryledstatus, batteryplus and powerstate (power.c
#     and simpleton-thermal own those jobs). The packages stay in the
#     build; a mask costs nothing and is one line to lift.
#   - the Wi-Fi driver (rtw88_8723ds on the RGB20SX) is blacklisted; the
#     Bluetooth stack shares no module with it.
#   - systemd-resolved's LLMNR responder is off.
#   - /usr/share/licenses (every licence text of the tree) and the written
#     source offer, /usr/share/simpleton/SOURCE-OFFER.txt (round 2).
#   - simpleton-profile.service writes one journal line at boot listing
#     the masks, so "why doesn't X run" has its answer.
# MPD's own lines (loopback only, zeroconf off) are in simpleton-mpd-config.

PKG_NAME="simpleton-system"
PKG_VERSION=""
PKG_LICENSE="GPL"
PKG_SITE=""
PKG_URL=""
PKG_DEPENDS_TARGET="toolchain"
PKG_LONGDESC="Simpleton: the appliance profile - unneeded services masked, Wi-Fi off, LLMNR off."
PKG_TOOLCHAIN="manual"

SIMPLETON_MASKS="smbd.service nmbd.service wsdd2.service rpcbind.service rpcbind.socket \
                 avahi-daemon.service avahi-daemon.socket NetworkManager.service iwd.service \
                 network-online.service volume-fixup.service batteryledstatus.service \
                 batteryplus.service powerstate.service"

makeinstall_target() {
  # the masks themselves live in projects/ROCKNIX/filesystem (see above);
  # SIMPLETON_MASKS is the list the boot line checks against
  mkdir -p ${INSTALL}/usr/lib/systemd/resolved.conf.d
  cp ${PKG_DIR}/scripts/10-simpleton-llmnr.conf ${INSTALL}/usr/lib/systemd/resolved.conf.d/

  # the licence texts of everything in the image, and the written source
  # offer the GPL asks for, on the system itself (image round 2, 29 Oct 2026)
  mkdir -p ${INSTALL}/usr/share/licenses ${INSTALL}/usr/share/simpleton
  cp ${ROOT}/licenses/*.txt ${INSTALL}/usr/share/licenses/
  cp ${ROOT}/LICENSE.md ${INSTALL}/usr/share/licenses/ROCKNIX-LICENSE.md
  cp ${PKG_DIR}/scripts/SOURCE-OFFER.txt ${INSTALL}/usr/share/simpleton/SOURCE-OFFER.txt
  ln -sf ../simpleton/SOURCE-OFFER.txt ${INSTALL}/usr/share/licenses/SOURCE-OFFER.txt

  mkdir -p ${INSTALL}/usr/bin
  cp ${PKG_DIR}/scripts/simpleton-profile ${INSTALL}/usr/bin
  chmod 0755 ${INSTALL}/usr/bin/simpleton-profile
  sed -i "s|@MASKS@|${SIMPLETON_MASKS}|" ${INSTALL}/usr/bin/simpleton-profile
}

post_install() {
  enable_service "simpleton-profile.service"
}
