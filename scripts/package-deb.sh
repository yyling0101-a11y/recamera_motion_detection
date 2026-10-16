#!/usr/bin/env bash
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${VERSION:-2.1.2}"
BUILD_DIR="${BUILD_DIR:-${HOME}/work/sscma-example-sg200x/solutions/motion_detection/build}"
SG200X_SDK_PATH="${SG200X_SDK_PATH:-${HOME}/work/sdk_dl/sg2002_recamera_emmc}"
RTSP_LIB="${RTSP_LIB:-${SG200X_SDK_PATH}/cvi_rtsp/install/lib/libcvi_rtsp.so}"
OUTPUT_DIR="${OUTPUT_DIR:-${REPO_DIR}/dist}"
PACKAGE="recamera-motion"
ARCH="riscv64"

BIN="${BUILD_DIR}/motion_detection"
[[ -x "$BIN" ]] || { echo "missing target binary: $BIN (run scripts/build.sh first)" >&2; exit 1; }
[[ -f "$RTSP_LIB" ]] || { echo "missing target RTSP library: $RTSP_LIB" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
ROOT="${WORK}/${PACKAGE}_${VERSION}_${ARCH}"
install -d "$ROOT/DEBIAN" "$ROOT/opt/recamera-motion/lib" "$ROOT/etc/init.d"
install -m 0755 "$BIN" "$ROOT/opt/recamera-motion/motion_detection"
install -m 0644 "$RTSP_LIB" "$ROOT/opt/recamera-motion/lib/libcvi_rtsp.so"
install -m 0755 "$REPO_DIR/deploy/rootfs/opt/recamera-motion/recamera-motion" "$ROOT/opt/recamera-motion/recamera-motion"
install -m 0755 "$REPO_DIR/deploy/rootfs/etc/init.d/S96recamera-motion" "$ROOT/etc/init.d/K92recamera-motion"

cat > "$ROOT/DEBIAN/control" <<EOF
Package: ${PACKAGE}
Version: ${VERSION}
Architecture: ${ARCH}
Maintainer: Seeed Studio <support@seeed.cc>
Description: Region-based motion detection for reCamera
 Detects motion in selected camera regions and provides HTTP, snapshot,
 RTSP, and webhook interfaces.
EOF

cat > "$ROOT/DEBIAN/postinst" <<'EOF'
#!/bin/sh
chmod 0755 /opt/recamera-motion/motion_detection
chmod 0755 /opt/recamera-motion/recamera-motion
chmod 0755 /etc/init.d/K92recamera-motion
exit 0
EOF
chmod 0755 "$ROOT/DEBIAN/postinst"

mkdir -p "$OUTPUT_DIR"
dpkg-deb --build --root-owner-group -Zgzip -z6 "$ROOT" "$OUTPUT_DIR/${PACKAGE}_${VERSION}_${ARCH}.deb"
sha256sum "$OUTPUT_DIR/${PACKAGE}_${VERSION}_${ARCH}.deb"
