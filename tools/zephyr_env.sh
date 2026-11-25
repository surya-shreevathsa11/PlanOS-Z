# Source me:  . tools/zephyr_env.sh
# Activates the repo-local venv + Zephyr workspace + SDK created by tools/zephyr_setup.sh.
_pz_root="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="$_pz_root/.venv/bin:$PATH"
export ZEPHYR_BASE="$_pz_root/.zephyr-ws/zephyr"
export ZEPHYR_SDK_INSTALL_DIR="$_pz_root/.zephyr-sdk"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
unset _pz_root
