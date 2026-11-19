#!/usr/bin/env bash
# Builds and installs Rockchip MPP, librga (multi-core) and the GStreamer
# plugin gstreamer-rockchip (mppvideodec, mppjpegdec, ...) from source.
#
# For the Armbian vendor kernel (…-vendor-rk35xx) on RK3588 when no binary
# packages exist for the distribution (e.g. Ubuntu 26.04).
#
#   ./scripts/build-rockchip-mpp.sh            # build + install (asks for sudo)
#   SRC=~/rk-src ./scripts/build-rockchip-mpp.sh
set -euo pipefail

SRC="${SRC:-$HOME/rockchip-src}"
MIRROR="https://github.com/JeffyCN/mirrors.git"   # Rockchip developer's mirror
JOBS="$(nproc)"
SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

say() { printf '\n\033[1;36m== %s\033[0m\n' "$*"; }

case "$(uname -r)" in
  *vendor*|*rk35xx*|*rockchip-rk3588*) ;;
  *) echo "ВНИМАНИЕ: ядро $(uname -r) не похоже на vendor-ядро Rockchip; MPP работает только с ним." >&2 ;;
esac
[ -e /dev/mpp_service ] || echo "ВНИМАНИЕ: нет /dev/mpp_service — MPP-декодер в этом ядре недоступен." >&2

say "Зависимости сборки"
$SUDO apt-get update
$SUDO apt-get install -y git cmake meson ninja-build pkg-config build-essential libdrm-dev \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev

mkdir -p "$SRC"
fetch() {   # branch dir
  if [ -d "$SRC/$2/.git" ]; then
    git -C "$SRC/$2" fetch --depth 1 origin "$1" && git -C "$SRC/$2" reset --hard FETCH_HEAD
  else
    git clone --depth 1 -b "$1" "$MIRROR" "$SRC/$2"
  fi
}

say "1/3 Rockchip MPP"
fetch mpp mpp
cmake -S "$SRC/mpp" -B "$SRC/mpp/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
      -DRKPLATFORM=ON -DHAVE_DRM=ON -DBUILD_TEST=OFF
cmake --build "$SRC/mpp/build" -j"$JOBS"
$SUDO cmake --install "$SRC/mpp/build"
$SUDO ldconfig

say "2/3 librga (multi-core RGA for RK3588)"
RGA=enabled
if fetch linux-rga-multi rga; then
  rm -rf "$SRC/rga/build"
  if meson setup "$SRC/rga/build" "$SRC/rga" --prefix=/usr --buildtype=release \
        -Dlibdrm=true -Dlibrga_demo=false \
     && ninja -C "$SRC/rga/build" && $SUDO ninja -C "$SRC/rga/build" install; then
    $SUDO ldconfig
  else
    RGA=disabled
  fi
else
  RGA=disabled
fi
[ "$RGA" = disabled ] && echo "librga не собралась: плагин будет собран без RGA (декодирование работает)." >&2

say "3/3 gstreamer-rockchip (mppvideodec)"
fetch gstreamer-rockchip gst-rockchip
MULTIARCH="$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || echo aarch64-linux-gnu)"
OPTS=(--prefix=/usr --libdir="lib/$MULTIARCH" --buildtype=release
      -Drockchipmpp=enabled -Drga="$RGA" -Drkximage=disabled -Dkmssrc=disabled
      -Dgpu=disabled -Dvpxalphadec=disabled)
rm -rf "$SRC/gst-rockchip/build"
meson setup "$SRC/gst-rockchip/build" "$SRC/gst-rockchip" "${OPTS[@]}"
ninja -C "$SRC/gst-rockchip/build"
$SUDO ninja -C "$SRC/gst-rockchip/build" install

say "Права на устройства видеоблока (группа video)"
$SUDO tee /etc/udev/rules.d/99-rockchip-multimedia.rules >/dev/null <<'EOF'
KERNEL=="mpp_service", MODE="0660", GROUP="video"
KERNEL=="rga", MODE="0660", GROUP="video"
KERNEL=="vpu_service|hevc_service|rkvdec|rkvenc|vepu|h265e", MODE="0660", GROUP="video"
SUBSYSTEM=="dma_heap", MODE="0660", GROUP="video"
EOF
$SUDO udevadm control --reload-rules && $SUDO udevadm trigger
$SUDO usermod -aG video,render "${SUDO_USER:-$USER}" || true

# GStreamer caches the plugin list.
rm -f "$HOME"/.cache/gstreamer-1.0/registry.*.bin

say "Проверка"
if gst-inspect-1.0 mppvideodec >/dev/null 2>&1; then
  echo "mppvideodec: OK"
  gst-inspect-1.0 | grep -iE "mpp" || true
else
  echo "mppvideodec НЕ найден. Вывод для диагностики:" >&2
  GST_DEBUG=2 gst-inspect-1.0 "/usr/lib/$MULTIARCH/gstreamer-1.0/libgstrockchipmpp.so" 2>&1 | tail -20 >&2
  exit 1
fi
echo
echo "Готово. Если пользователь только что добавлен в группу video — выйдите из сеанса и войдите снова."
