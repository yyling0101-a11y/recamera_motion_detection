#!/usr/bin/env bash
# Build (if needed) and install motion_detection on a reCamera over SSH.
#
#   ./scripts/deploy.sh                       # root@192.168.42.1
#   ./scripts/deploy.sh --host root@192.168.4.5
#   ./scripts/deploy.sh --no-service          # copy files only
#   ./scripts/deploy.sh --uninstall
#
# Uses SSH key authentication. Copy your key first if needed:
#   ssh-copy-id root@192.168.42.1
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="root@192.168.42.1"
START_SERVICE=1
UNINSTALL=0
SSHD_OPTS=(-o BatchMode=yes -o StrictHostKeyChecking=no)

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host) HOST="$2"; shift 2 ;;
    --no-service) START_SERVICE=0; shift ;;
    --uninstall) UNINSTALL=1; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [[ $UNINSTALL -eq 1 ]]; then
  ssh "${SSHD_OPTS[@]}" "$HOST" '
    for init in /etc/init.d/[SK]96recamera-motion; do
      [ -f "$init" ] && "$init" stop >/dev/null 2>&1 || true
    done
    rm -f /etc/init.d/[SK]96recamera-motion
    rm -rf /opt/recamera-motion
    /etc/init.d/S92recamera-onvif start || true
  '
  echo "uninstalled; stock reCamera-onvif service restarted"
  exit 0
fi

SSCMA_EXAMPLE_PATH="${SSCMA_EXAMPLE_PATH:-$HOME/work/sscma-example-sg200x}"
SG200X_SDK_PATH="${SG200X_SDK_PATH:-$HOME/work/sdk_dl/sg2002_recamera_emmc}"
BIN="${BUILD_DIR:-$SSCMA_EXAMPLE_PATH/solutions/motion_detection}/build/motion_detection"
RTSP_LIB="$SG200X_SDK_PATH/cvi_rtsp/install/lib/libcvi_rtsp.so"

if [[ ! -f "$BIN" ]]; then
  "$REPO_DIR/scripts/build.sh"
fi
[[ -f "$RTSP_LIB" ]] || { echo "missing $RTSP_LIB" >&2; exit 1; }

TMP="/tmp/recamera-motion-$$"
ssh "${SSHD_OPTS[@]}" "$HOST" "mkdir -p $TMP/rootfs"
scp "${SSHD_OPTS[@]}" "$BIN" "$HOST:$TMP/motion_detection"
scp "${SSHD_OPTS[@]}" "$RTSP_LIB" "$HOST:$TMP/libcvi_rtsp.so"
scp -r "${SSHD_OPTS[@]}" "$REPO_DIR/deploy/rootfs/." "$HOST:$TMP/rootfs/"

ssh "${SSHD_OPTS[@]}" "$HOST" "
  set -e
  # A firmware restore can rename the boot script to K96recamera-motion, which
  # rcS never starts. Drop every variant before reinstalling as S96.
  for init in /etc/init.d/[SK]96recamera-motion; do
    [ -f \"\$init\" ] && \"\$init\" stop >/dev/null 2>&1 || true
  done
  rm -f /etc/init.d/[SK]96recamera-motion
  mkdir -p /opt/recamera-motion/lib
  install -m 0755 $TMP/motion_detection /opt/recamera-motion/motion_detection
  install -m 0644 $TMP/libcvi_rtsp.so /opt/recamera-motion/lib/libcvi_rtsp.so
  install -m 0755 $TMP/rootfs/opt/recamera-motion/recamera-motion /opt/recamera-motion/recamera-motion
  install -m 0755 $TMP/rootfs/etc/init.d/S96recamera-motion /etc/init.d/S96recamera-motion
  rm -rf $TMP
"

if [[ $START_SERVICE -eq 1 ]]; then
  ssh "${SSHD_OPTS[@]}" "$HOST" "/etc/init.d/S96recamera-motion restart"
  sleep 6
  ssh "${SSHD_OPTS[@]}" "$HOST" "/etc/init.d/S96recamera-motion status; tail -n 15 /var/log/recamera-motion.log"
fi
echo "deployed to $HOST"
