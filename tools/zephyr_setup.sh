#!/usr/bin/env bash
# Reproducible Zephyr bring-up for PlanOS-Z, entirely inside the repo (all paths gitignored):
#   .venv/         Python venv (planoszc + west + Zephyr python deps)
#   .zephyr-ws/    west workspace: zephyr v4.5.0 + only the modules we need (shallow)
#   .zephyr-sdk/   Zephyr SDK 1.0.1 (host tools incl. dtc + xtensa ESP32 toolchain)
# Needs: python3 (venv), git, cmake, ninja, wget, xz.  No sudo.  ~5 GB disk, ~10 min.
set -euo pipefail

ZEPHYR_REV="${ZEPHYR_REV:-v4.5.0}"
SDK_VER="${SDK_VER:-1.0.1}"
SDK_TOOLCHAINS="${SDK_TOOLCHAINS:-xtensa-espressif_esp32_zephyr-elf}"   # space separated
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "==> python venv + west"
[ -x .venv/bin/pip ] || python3 -m venv .venv
.venv/bin/pip install -q --upgrade pip
.venv/bin/pip install -q west -e "tools/planoszc[dev]"

echo "==> west workspace (.zephyr-ws, zephyr $ZEPHYR_REV)"
if [ ! -d .zephyr-ws/.west ]; then
  .venv/bin/west init -m https://github.com/zephyrproject-rtos/zephyr --mr "$ZEPHYR_REV" .zephyr-ws
  (cd .zephyr-ws && ../.venv/bin/west config manifest.project-filter -- '-.*,+zephyr,+cmsis,+cmsis_6,+picolibc,+hal_espressif')
fi
(cd .zephyr-ws && ../.venv/bin/west update --narrow -o=--depth=1)
.venv/bin/pip install -q -r .zephyr-ws/zephyr/scripts/requirements-base.txt "esptool>=5.0.2"

echo "==> Zephyr SDK $SDK_VER"
if [ ! -x .zephyr-sdk/setup.sh ]; then
  mkdir -p .zephyr-sdk-dl .zephyr-sdk
  tarball="zephyr-sdk-${SDK_VER}_linux-x86_64_minimal.tar.xz"
  wget -q -c -P .zephyr-sdk-dl "https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${SDK_VER}/${tarball}"
  tar -xf ".zephyr-sdk-dl/${tarball}" -C .zephyr-sdk --strip-components=1
fi
[ -d .zephyr-sdk/hosttools/sysroots ] || .zephyr-sdk/setup.sh -h
for tc in $SDK_TOOLCHAINS; do
  [ -d ".zephyr-sdk/gnu/$tc" ] || yes | .zephyr-sdk/setup.sh -t "$tc"
done
rm -rf .zephyr-sdk-dl

cat <<MSG

Done. Next:
  . tools/zephyr_env.sh
  make native-sim-demo     # ACCEPT / REJECT / REJECT on the real Zephyr runtime (native_sim)
  make esp32-build         # cross-build for esp32_devkitc/esp32/procpu
MSG
