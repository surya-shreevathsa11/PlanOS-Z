# Roadmap

Phases follow the [master spec](docs/MASTER_SPEC.txt) §12. Status is what the repository
does today; "partial" says exactly what is missing.

| Phase | Goal | Status |
|---|---|---|
| 0 | Spec freeze, architecture, error codes | **done** (`ARCHITECTURE.md`, `docs/error_codes.md`) |
| 1 | Skeleton + `native_sim` bring-up, shell command, CI build | **done** — builds and runs; `planosz status` shell command; CI job written (not yet run on GitHub) |
| 2 | IDL + codegen + registry | **done** — 7 contracts, JSON Schemas, generated C table with arg ranges, `planosz skills` |
| 3 | Validated executor + ingress | **partial** — plans enter as compiled IR registered by the app and selected from the shell; no UART/network blob upload yet |
| 4 | Admission control (the soul) | **done** — all reject codes, host and device, tested and cross-checked |
| 5 | DAG / parallel scheduling | **partial** — host executor forks/joins in virtual time (critical-path timing); device executor runs topologically, sequentially (sum-of-WCET timing) |
| 6 | Trace, replay, fault injection | **partial** — NDJSON + golden trace + fault injection (sensor fail, slow, backend down); no replay harness, no on-device ring buffer |
| 7 | Tooling polish | **partial** — `check/admit/run/compile/skills/codegen`, `west planosz` wrapper; no `west planosz skill new` scaffolder |
| 8 | ESP32 backend on hardware | **partial** — cross-builds, real GPIO outputs, simulated sensors; **never flashed** |
| 9 | Stretch | not started |

## Next (in order)

1. **Device recomputes WCET + resource masks from its skill table** (closes the host-trust gap
   Opus flagged): codegen emits evaluable WCET models and resource templates into C; admission
   ignores host-baked `wcet_ms` / `excl_mask` / `shared_mask`; add tampered-IR unit tests.
2. **Flash an ESP32** and record a real run: wire three LEDs, verify the overlay pins, add the
   result and a photo/log to `docs/esp32_port.md`. (Closes phase 8's exit criterion.)
3. **Device-side parallel executor**: one ready-queue, a small worker pool (Zephyr threads or
   work queue), resource masks acquired with timeout; add a native_sim test that proves the
   fork overlaps in time; then flip `PLANOSZ_EXECUTOR_PARALLEL` so device admission uses the
   critical-path model (needs a response-time argument for the core/priority setup).
4. **Replay harness**: `planoszc replay trace.ndjson` re-checks causality/resource overlap and
   diffs against a golden; device trace ring buffer exported over the shell.
5. **Real sensors on ESP32** (ADC for `pack_voltage`, internal temperature sensor or an I2C part
   for `coolant_proxy`), and measure the real latency of every skill to replace guessed WCETs
   with measured-plus-margin values (record the method).
6. **Wire ingress**: upload an IR blob over the shell/UART with a version byte
   (`REJECT_UNSUPPORTED_IR`) and a CRC.
7. **Twister**: move the `native_sim` decisions into a Zephyr twister suite.
8. `west planosz skill new` scaffolder (IDL + handler stub + test stub).

## Stretch (after the above)

CBOR IR on the wire · power-budget accounting using `power_class` · multi-plan queue with
priorities · local planner plugin that emits plans (still gated by the same admission) · signed
skill packs · multi-node simulation.

## Non-goals

LLM chatbot features, cloud dependencies, a single-purpose gadget app, a Zephyr tutorial.
