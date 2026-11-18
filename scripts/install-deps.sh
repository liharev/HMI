#!/usr/bin/env bash
# Installs build and runtime dependencies of HMI VMS on Armbian / Debian / Ubuntu.
#
#   ./scripts/install-deps.sh              # Qt 6 + GStreamer
#   ./scripts/install-deps.sh --rockchip   # + Rockchip MPP/RGA GStreamer plugin (Ubuntu-based Armbian, vendor kernel)
set -euo pipefail

SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

PKGS=(
  build-essential cmake ninja-build pkg-config
  qt6-base-dev qt6-declarative-dev qt6-multimedia-dev
  qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-layouts
  qml6-module-qtquick-window qml6-module-qtquick-dialogs qml6-module-qtquick-templates
  qml6-module-qtqml-workerscript qml6-module-qtmultimedia
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
  gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good
  gstreamer1.0-plugins-bad gstreamer1.0-libav gstreamer1.0-gl
)

$SUDO apt-get update
$SUDO apt-get install -y "${PKGS[@]}"

# Zero-copy GL output (qml6glsink). Not packaged on every release (e.g. Debian 12):
# without it the application falls back to Qt Multimedia output automatically.
if ! $SUDO apt-get install -y gstreamer1.0-qt6; then
  echo "WARNING: gstreamer1.0-qt6 is not available - the Qt Multimedia output will be used." >&2
fi

if [ "${1:-}" = "--rockchip" ]; then
  # Rockchip MPP (VPU) + RGA plugins for the vendor (BSP) kernel on Ubuntu 22.04/24.04 based Armbian.
  . /etc/os-release
  if [ "${ID:-}" = "ubuntu" ]; then
    $SUDO apt-get install -y software-properties-common
    $SUDO add-apt-repository -y ppa:liujianfeng1994/rockchip-multimedia
    $SUDO apt-get update
    $SUDO apt-get install -y rockchip-multimedia-config gstreamer1.0-rockchip1 || \
      $SUDO apt-get install -y gstreamer1.0-rockchip1
  else
    echo "Debian based image: build gstreamer-rockchip from source (see README, section 'Аппаратное декодирование')." >&2
  fi
fi

# Access to /dev/dri (GPU), /dev/mpp_service, /dev/rga, /dev/video* (VPU).
$SUDO usermod -aG video,render "${SUDO_USER:-$USER}" || true

echo
echo "Hardware decoders found:"
for f in mppvideodec mppjpegdec v4l2slh264dec v4l2slh265dec v4l2h264dec v4l2h265dec; do
  gst-inspect-1.0 "$f" >/dev/null 2>&1 && echo "  + $f"
done
gst-inspect-1.0 qml6glsink >/dev/null 2>&1 && echo "qml6glsink: OK (zero-copy GL output)" || echo "qml6glsink: missing"
