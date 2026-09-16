# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 Simpleton

PKG_NAME="simpleton-gadget-default"
PKG_VERSION=""
PKG_LICENSE="GPL"
PKG_SITE=""
PKG_URL=""
PKG_DEPENDS_TARGET="toolchain"
PKG_LONGDESC="Simpleton: bring up USB gadget networking on every boot (dev-mode access)."
PKG_TOOLCHAIN="manual"

makeinstall_target() {
  mkdir -p ${INSTALL}/usr/lib/autostart/common
  cp ${PKG_DIR}/sources/autostart/common/* ${INSTALL}/usr/lib/autostart/common
  chmod 0755 ${INSTALL}/usr/lib/autostart/common/*
}
