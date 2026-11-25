# ESP32 port

**Status: cross-builds; real GPIO outputs; simulated sensors; never flashed on hardware.**
This page records what exists and the exact steps to close the gap.

## What exists

* Board: `esp32_devkitc/esp32/procpu` (Zephyr 4.5 board-qualifier syntax: board / SoC / CPU
  cluster; the ESP32 has PRO and APP cores, we run on PRO).
* Toolchain: Zephyr SDK 1.0.1 `xtensa-espressif_esp32_zephyr-elf` (installed by
  `tools/zephyr_setup.sh`), HAL from the `hal_espressif` module, image creation by `esptool` ≥ 5.
* Backend: `backends/esp32/esp32_backend.c` implements the 6-function HAL.
  Outputs `warn_lamp`, `cooling_fan`, `buzzer` are `gpio_dt_spec`s resolved from devicetree
  aliases `warn-lamp`, `cooling-fan`, `buzzer`, defined by
  `apps/planosz_node/boards/esp32_devkitc_procpu.overlay` (GPIO2 / GPIO4 / GPIO5).
* Selected automatically: `PLANOSZ_BACKEND_ESP32` is the Kconfig default on ESP32 SoCs.
* Measured build (v0.1): FLASH 174,848 B (4.2 % of 4 MB), DRAM 16,568 B (8.4 %).

```bash
make zephyr-setup
make esp32-build          # → build/esp32/zephyr/zephyr.bin
```

## What is simulated / missing

| Skill | ESP32 today |
|---|---|
| `actuator.pulse` | real GPIO (if the overlay pins match your wiring) |
| `sensor.read` | **simulated constants** — TODO ADC1 channel / internal temperature sensor / I2C part |
| `power.idle` | `k_msleep` — TODO Zephyr PM light/deep sleep |
| `actuator.contactor` | delay only (no output) — hazardous skill intentionally not wired |
| `sys.info` | uptime/board string |

## Bring-up checklist (first hardware session)

1. Wire LEDs (with resistors) to GPIO2/4/5, or edit the overlay.
2. `. tools/zephyr_env.sh && west flash -d build/esp32` (esptool over USB; udev rules / dialout
   group may be needed). Console: `west espressif monitor` or `picocom /dev/ttyUSB0 -b 115200`.
3. `planosz plans`, `planosz run accept` — expect the same admit event as `native_sim` and a
   visibly pulsing lamp. `planosz run conflict` must reject with `REJECT_RESOURCE_CONFLICT`.
4. **Measure** `sensor.read` and `actuator.pulse` latency with a scope or `k_uptime_get`; replace
   the guessed WCET expressions with measured-max-plus-margin and write the method in the IDL
   comments. The `native_sim` tick lesson (10 ms default tick made `k_msleep(5)` take 20 ms) is
   the reason this step matters; the app already sets `CONFIG_SYS_CLOCK_TICKS_PER_SEC=1000`.
5. Record the log in this file. Only then update the README status line.

## Design rule

Hardware is a **port, not a rewrite**: nothing under `runtime/` or `skills/` mentions ESP32.
If porting requires touching them, the HAL is wrong — fix the HAL.
