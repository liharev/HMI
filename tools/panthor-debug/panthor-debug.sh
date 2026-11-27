#!/usr/bin/env bash
# Instrumented build of the Mali "panthor" GPU driver modules (panthor,
# drm_gpuvm, drm_exec) of the Armbian vendor kernel 6.1.115-vendor-rk35xx,
# to find the use-after-free that oopses the kernel in
# drm_gpuvm_prepare_objects() when decoder DMA buffers are imported by the GPU.
#
# The modules are built from the exact sources of the running kernel
# (armbian/linux-rockchip, branch rk-6.1-rkr5.1) plus tools/panthor-debug/debug.patch,
# installed into /lib/modules/<kernel>/updates (preferred over the original
# modules) and removed again with "uninstall". The kernel itself is not changed.
#
#   sudo apt install linux-headers-vendor-rk35xx
#   tools/panthor-debug/panthor-debug.sh build
#   sudo tools/panthor-debug/panthor-debug.sh install     # then reboot
#   tools/panthor-debug/panthor-debug.sh status
#   sudo tools/panthor-debug/panthor-debug.sh uninstall   # then reboot
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# Under sudo $HOME is /root: use the home of the user who called sudo.
USER_HOME="$HOME"
if [ -n "${SUDO_USER:-}" ]; then
  USER_HOME="$(getent passwd "$SUDO_USER" | cut -d: -f6)"
fi
WORK="${WORK:-$USER_HOME/panthor-debug}"
KREL="$(uname -r)"
EXPECTED="6.1.115-vendor-rk35xx"
BRANCH="rk-6.1-rkr5.1"
RAW="https://raw.githubusercontent.com/armbian/linux-rockchip/$BRANCH"
DEST="/lib/modules/$KREL/updates/hmi-panthor-debug"
PANTHOR_FILES="panthor_devfreq.c panthor_devfreq.h panthor_device.c panthor_device.h panthor_drv.c
  panthor_fw.c panthor_fw.h panthor_gem.c panthor_gem.h panthor_gpu.c panthor_gpu.h panthor_heap.c
  panthor_heap.h panthor_mmu.c panthor_mmu.h panthor_regs.h panthor_sched.c panthor_sched.h"

say() { printf '\n\033[1;36m== %s\033[0m\n' "$*"; }
die() { echo "ОШИБКА: $*" >&2; exit 1; }

cmd_fetch() {
  [ "$KREL" = "$EXPECTED" ] || echo "ВНИМАНИЕ: ядро $KREL, а исходники взяты для $EXPECTED" >&2
  say "Исходники ядра $BRANCH"
  rm -rf "$WORK/src"
  mkdir -p "$WORK/src/panthor"
  for f in $PANTHOR_FILES; do
    curl -fsSL -o "$WORK/src/panthor/$f" "$RAW/drivers/gpu/drm/panthor/$f" || die "не удалось скачать $f"
  done
  curl -fsSL -o "$WORK/src/drm_gpuvm.c" "$RAW/drivers/gpu/drm/drm_gpuvm.c" || die "drm_gpuvm.c"
  curl -fsSL -o "$WORK/src/drm_exec.c" "$RAW/drivers/gpu/drm/drm_exec.c" || die "drm_exec.c"
  (cd "$WORK/src" && patch -p1 < "$HERE/debug.patch") || die "отладочная правка не применилась"
  cp "$HERE/hmi_debug.h" "$WORK/src/"
  cat > "$WORK/src/Kbuild" <<'KB'
ccflags-y += -include $(src)/hmi_debug.h
obj-m := panthor.o drm_gpuvm.o drm_exec.o
panthor-y := panthor/panthor_devfreq.o panthor/panthor_device.o panthor/panthor_drv.o \
	panthor/panthor_fw.o panthor/panthor_gem.o panthor/panthor_gpu.o panthor/panthor_heap.o \
	panthor/panthor_mmu.o panthor/panthor_sched.o
KB
}

cmd_build() {
  [ -d "/lib/modules/$KREL/build" ] || die "нет заголовков ядра: sudo apt install linux-headers-vendor-rk35xx"
  # Always start from fresh sources: the debug patch changes between rounds.
  cmd_fetch
  say "Сборка модулей для $KREL"
  # The kernel was built with GCC 14: use the same major version if present
  # (sudo apt install gcc-14), otherwise the default compiler.
  local cc=()
  if command -v gcc-14 >/dev/null; then
    cc=(CC=gcc-14)
    echo "Компилятор: gcc-14 (как у ядра)"
  else
    echo "gcc-14 не найден: используется $(gcc -dumpfullversion). Для точного совпадения: sudo apt install gcc-14"
  fi
  make -C "/lib/modules/$KREL/build" M="$WORK/src" "${cc[@]}" -j"$(nproc)" modules
  ls -la "$WORK/src"/*.ko
}

cmd_install() {
  [ "$(id -u)" -eq 0 ] || die "запустите через sudo"
  ls "$WORK/src"/panthor.ko "$WORK/src"/drm_gpuvm.ko "$WORK/src"/drm_exec.ko >/dev/null || die "сначала build"
  say "Установка в $DEST"
  mkdir -p "$DEST"
  cp "$WORK/src"/panthor.ko "$WORK/src"/drm_gpuvm.ko "$WORK/src"/drm_exec.ko "$DEST/"
  depmod -a "$KREL"
  command -v update-initramfs >/dev/null && update-initramfs -u -k "$KREL" || true
  echo "Готово. Перезагрузите систему: sudo reboot"
}

cmd_uninstall() {
  [ "$(id -u)" -eq 0 ] || die "запустите через sudo"
  rm -rf "$DEST"
  depmod -a "$KREL"
  command -v update-initramfs >/dev/null && update-initramfs -u -k "$KREL" || true
  echo "Отладочные модули удалены. Перезагрузите систему: sudo reboot"
}

cmd_status() {
  for m in panthor drm_gpuvm drm_exec; do
    printf '%-10s %s  %s\n' "$m" "$(modinfo -n $m 2>/dev/null)" \
      "$(v=$(modinfo -F hmi_debug $m 2>/dev/null); [ -n "$v" ] && echo "[отладочный, версия $v]")"
  done
  [ -e /sys/module/panthor ] && echo "panthor загружен" || echo "panthor НЕ загружен"
}

case "${1:-}" in
  fetch) cmd_fetch ;;
  build) cmd_build ;;
  install) cmd_install ;;
  uninstall) cmd_uninstall ;;
  status) cmd_status ;;
  *) sed -n '2,16p' "$0"; exit 1 ;;
esac
