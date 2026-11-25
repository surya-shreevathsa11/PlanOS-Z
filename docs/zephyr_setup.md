# Zephyr setup and bring-up

Verified on Ubuntu 24.04 (x86_64), Python 3.12, no sudo. Pinned versions: **Zephyr v4.5.0**,
**Zephyr SDK 1.0.1**, west 1.5.0.

## Option A - repo-local workspace (what `make zephyr-setup` does)

Everything lands in gitignored directories inside the repo, so `make distclean` removes it all.

```bash
make zephyr-setup        # = tools/zephyr_setup.sh
. tools/zephyr_env.sh    # PATH, ZEPHYR_BASE, ZEPHYR_SDK_INSTALL_DIR, ZEPHYR_TOOLCHAIN_VARIANT=zephyr
make native-sim-demo     # build + run ACCEPT / REJECT / REJECT on native_sim
make esp32-build         # build for esp32_devkitc/esp32/procpu
```

| Directory | Contents | Size |
|---|---|---|
| `.venv/` | planoszc, west, Zephyr python deps, esptool | ~100 MB |
| `.zephyr-ws/` | `west init -m zephyr --mr v4.5.0`, project-filter limited to `zephyr cmsis cmsis_6 picolibc hal_espressif`, `west update --narrow --depth=1` | ~1.8 GB |
| `.zephyr-sdk/` | SDK minimal + host tools (dtc, ...) + `xtensa-espressif_esp32_zephyr-elf` | ~1.4 GB |
| `build/` | app build dirs | ~85 MB |

The script does exactly this (so you can do it by hand):

```bash
python3 -m venv .venv && .venv/bin/pip install west -e "tools/planoszc[dev]"
.venv/bin/west init -m https://github.com/zephyrproject-rtos/zephyr --mr v4.5.0 .zephyr-ws
cd .zephyr-ws
west config manifest.project-filter -- '-.*,+zephyr,+cmsis,+cmsis_6,+picolibc,+hal_espressif'
west update --narrow -o=--depth=1
pip install -r zephyr/scripts/requirements-base.txt "esptool>=5.0.2"
cd ..
# SDK (https://github.com/zephyrproject-rtos/sdk-ng/releases/tag/v1.0.1), minimal bundle:
tar -xf zephyr-sdk-1.0.1_linux-x86_64_minimal.tar.xz -C .zephyr-sdk --strip-components=1
.zephyr-sdk/setup.sh -h                                        # host tools incl. dtc
.zephyr-sdk/setup.sh -t xtensa-espressif_esp32_zephyr-elf      # ESP32 compiler
```

`native_sim` only needs the host tools + host `gcc`; the Xtensa toolchain is only for ESP32.
Without the SDK you can use `ZEPHYR_TOOLCHAIN_VARIANT=host` plus a system `dtc`
(`apt install device-tree-compiler`) — that is what CI does.

## Option B - standard west T2 workspace (`west.yml`)

This repo is a west *manifest repository* (`west.yml` imports Zephyr v4.5.0 and allow-lists
only the modules we need):

```bash
mkdir planosz-ws && cd planosz-ws
git clone <repo-url> planos-z
west init -l planos-z
west update --narrow -o=--depth=1
pip install -r zephyr/scripts/requirements-base.txt
west build -b native_sim/native/64 planos-z/apps/planosz_node -- -DEXTRA_CONF_FILE=demo.conf
./build/zephyr/zephyr.exe < /dev/null
west planosz admit planos-z/examples/missions/02_deadline_reject.yaml    # extension command
```

(This flow was verified: `west update`, module list, and `west planosz` all work.)

## Build variants of `apps/planosz_node`

| Goal | Command (from `.zephyr-ws`) |
|---|---|
| Non-interactive demo (no shell, runs 3 plans, exits) | `west build -b native_sim/native/64 -d ../build/native_sim_demo ../apps/planosz_node -- -DEXTRA_CONF_FILE=demo.conf` |
| Interactive shell on a PTY | `west build -b native_sim/native/64 -d ../build/native_sim ../apps/planosz_node` → run `build/native_sim/zephyr/zephyr.exe`, attach to the printed `/dev/pts/N` |
| Interactive shell on stdin/stdout (pipes, CI) | add `-- -DEXTRA_CONF_FILE=shell_stdio.conf` |
| ESP32 | `west build -b esp32_devkitc/esp32/procpu -d ../build/esp32 ../apps/planosz_node` |

Shell commands: `planosz status | skills | plans | admit <plan> | run <plan>` with plans
`accept`, `deadline`, `conflict`.

## Notes and gotchas seen while bringing this up

* **Tick rate matters.** With native_sim's default 10 ms tick `k_msleep(5)` takes 20 ms and the
  5-pulse lamp job blew its WCET (`EXEC_TIMEOUT`). `prj.conf` sets `CONFIG_SYS_CLOCK_TICKS_PER_SEC=1000`.
* **PTY in sandboxes/containers.** native_sim's default shell UART needs a PTY; where none exists
  (`Could not open a new PTY for the UART`) use `shell_stdio.conf` or `demo.conf`.
* **ESP32 builds need `esptool>=5.0.2`** in the Python environment (`west packages pip --install`
  also installs it).
* **ESP32 radio blobs** (`west blobs fetch hal_espressif`) are *not* needed: PlanOS-Z uses no
  Wi-Fi/Bluetooth.
* If `ZEPHYR_BASE` is exported from a previous workspace, west will look there; unset it when
  switching between workspaces.
* `west update` without `--narrow` fetches full history (several GB more).
