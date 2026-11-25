# Roadmap

Phases follow the [master spec](docs/MASTER_SPEC.txt) §12. Status is what the repository
does today; "partial" says exactly what is missing.

| Phase | Goal | Status |
|---|---|---|
| 0 | Spec freeze, architecture, error codes | **done** (`ARCHITECTURE.md`, `docs/error_codes.md`) |
| 1 | Skeleton + `native_sim` bring-up, shell command, CI build | **done** - builds and runs; shell commands; CI runs host tests, native_sim demo, replay and the shell upload test |
| 2 | IDL + codegen + registry | **done** - 7 contracts, JSON Schemas, generated C table with arg ranges, per-skill WCET functions and resource claim templates |
| 3 | Validated executor + ingress | **done** - embedded plans, shell commands, versioned/CRC IR upload over the shell (`planosz load`, `REJECT_UNSUPPORTED_IR`) into an ephemeral slot |
| 4 | Admission control (the soul) | **done** - all reject codes, host and device; the device recomputes WCET + resource masks from its own table; tamper tests |
| 5 | DAG / parallel scheduling | **done** - host virtual-time executor; device coordinator + worker threads (Zephyr threads, pthreads under host tests); worker-count-aware admission bound; overlap proven on native_sim |
| 6 | Trace, replay, fault injection | **done except the ring buffer** - NDJSON, goldens, fault injection, `planoszc replay` (causality/resources/WCET/deadline/overlap) used in CI; no on-device ring buffer |
| 7 | Tooling polish | **partial** - `check/admit/run/compile/replay/skills/codegen`, `west planosz` wrapper; no `west planosz skill new` scaffolder |
| 8 | ESP32 backend on hardware | **partial** - cross-builds, real GPIO outputs, simulated sensors; **never flashed (needs a board)** |
| 9 | Stretch | not started |

## Next (in order)

1. **Flash an ESP32** and record a real run: wire three LEDs, verify the overlay pins, run
   `planosz run parallel` and an upload, replay the console log, add the result and a photo/log to
   `docs/esp32_port.md`. (Closes phase 8's exit criterion; the only remaining hardware item.)
2. **Real sensors on ESP32** (ADC for `pack_voltage`, internal temperature sensor or an I2C part
   for `coolant_proxy`), and **measure** the real latency of every skill to replace declared
   WCETs with measured-plus-margin values (record the method). Check the worker-count bound on
   the dual-core target (priorities, preemption by the shell/logging threads).
3. **Preemptible skills**: today a skill is cooperative, so a watchdog can only report an overrun
   when the call returns. HAL calls with timeouts would make `EXEC_TIMEOUT` hard.
4. **Device trace ring buffer** exported over the shell (post-mortem replay without a live console).
5. **Twister**: move the `native_sim` decisions into a Zephyr twister suite.
6. `west planosz skill new` scaffolder (IDL + handler stub + test stub).

## Stretch (after the above)

CBOR IR on the wire · power-budget accounting using `power_class` · multi-plan queue with
priorities · local planner plugin that emits plans (still gated by the same admission) · signed
skill packs · multi-node simulation.

## Non-goals

LLM chatbot features, cloud dependencies, a single-purpose gadget app, a Zephyr tutorial.
