# PlanOS-Z architecture

This document describes what the code in this repository actually does. Where the code is
behind the [master spec](docs/MASTER_SPEC.txt), the divergence is listed in
[§11](#11-spec-vs-code).

## 1. Goal and principles

PlanOS-Z accepts **jobs** (a DAG of skill calls) and decides, before any side effect,
whether the device can and may run them. Principles that shape the code:

* **Contracts, not code, describe capabilities.** A skill is data (IDL) + a handler.
* **Reject early, with a stable code.** Admission is a pure function of
  (plan, contracts, policy) → ACCEPT | REJECT(code, detail).
* **The device does not trust the host's timing or resource claims.** The host compiles a
  plan to IR; the device re-checks structure, args, permissions and safety class against its own
  registry **and recomputes every step's WCET and resource masks from its own generated skill
  table** (`planosz_registry_derive`). The `wcet_ms` / `excl_mask` / `shared_mask` / `mutex_mask`
  fields in the IR are advisory: they are ignored for every decision, and a disagreement is
  only reported (`ir_wcet_mismatch` in the admit record). Branch exclusivity is also derived on
  the device from the `when` fields. What the device *does* still trust is the registry it was
  built with and its own HAL; WCETs are declared budgets, not measurements (see §5).
* **Core knows nothing about boards.** A narrow HAL (6 functions) is the only seam.
* **Everything observable.** One NDJSON event per admit/start/end/skip/error.
* **Deterministic first.** Host executor uses virtual time; `native_sim` runs in simulated
  time; golden traces are compared in CI.

## 2. Component map

```
idl/skills/*.yaml ──► planoszc.idl (load + validate vs contract.schema.json)
                          │                          │
                          │                          └─► planoszc.codegen ─► skill_table.gen.{h,c}
                          │                              (arg ranges, per-skill C WCET function,
                          │                               resource claim templates, resource bit space)
 plan.yaml ─► planoszc.admission.admit() ──ACCEPT──► planoszc.executor.run()  (host, virtual time)
                          │                                   │ events
                          │ ACCEPT/REJECT                     ▼
                          │                          planoszc.trace (NDJSON) ─► planoszc.replay
                          └─► planoszc.compile ──► IR (JSON / C header) ─► planoszc.wire (IR v1 bytes + CRC)
                                                        │                         │ base64 over UART
 ┌──────────────────────────────── Zephyr app (apps/planosz_node) ──────────────────────────────┐
 │ runtime/src/ingress   plan registry, IR upload slot (ir_wire.c), serialised submit,          │
 │                       shell: planosz status|skills|plans|admit|run|load begin/data/end       │
 │        ▼                                                                                      │
 │ runtime/src/admission  planosz_admit(IR, policy): re-derives WCET + masks from the skill     │
 │        │ ACCEPT        table; timing bound depends on policy.workers                        │
 │        ▼                                                                                      │
 │ runtime/src/scheduler  planosz_exec_run(): coordinator + N worker threads over the ready set │
 │   ├─ runtime/src/osal       mutex / sem / thread (Zephyr kernel on device, pthreads on host) │
 │   ├─ runtime/src/resource   exclusive bit-set + refcounted shared claims                     │
 │   ├─ skills/src/skills.c    one dispatch on the generated skill index (re-entrant)           │
 │   └─ runtime/src/trace      NDJSON through a pluggable sink, emitted under the exec lock     │
 │ runtime/include/planosz/hal.h  ◄── backends/native_sim | backends/esp32                      │
 └───────────────────────────────────────────────────────────────────────────────────────────────┘
```

## 3. Skill contracts (IDL)

One YAML per skill in `idl/skills/`, validated against `idl/schemas/contract.schema.json`
(see [docs/skill_idl.md](docs/skill_idl.md)). A contract declares: `skill_id`, `version`,
typed `args` (ranges/enums/defaults), `returns`, `resources` (each `exclusive` or `shared`,
names may reference args like `out_{channel}`), `wcet_ms` (an arithmetic expression over the
args), optional `typical_ms` (used only by the simulator), `power_class`, `safety_class`,
`permissions`.

Expressions are parsed with Python's `ast` against a strict whitelist (numbers, names,
`+ - * / // %`, comparisons, `a if c else b`, `min`, `max`) — never `eval`. Contracts are
data.

`planoszc codegen` emits `skill_table.gen.{h,c}`: skill indices (sorted by id), permission
bit positions, and for every skill the permission mask, safety/power class and **per-argument
ranges** (enums become `0..n-1`, floats milli-units) that the on-device admission uses to
re-validate arguments. A test fails if the committed generated files are stale.

Current skills: `sensor.read`, `actuator.pulse`, `wait`, `power.idle`, `sys.info`,
`diag.report`, `actuator.contactor` (HAZARDOUS, exists to demonstrate `REJECT_SAFETY_CLASS`).

## 4. Plans

YAML/JSON, validated against `idl/schemas/plan.schema.json` ([docs/plan_language.md](docs/plan_language.md)):
`plan_id`, `deadline_ms`, `permissions[]`, `steps[]` each with `id`, `action`, `args`,
`needs[]` (the DAG), optional `confirm`, optional `when` (`{ref: step.field, op, value}`).
Steps with no path between them are *concurrent*; that is the only notion of parallelism.

## 5. Admission control

`planoszc/admission.py` (host) and `runtime/src/admission/admission.c` (device) implement
the same four stages; the **first failing stage chooses the primary code**.

| Stage | Checks | Codes |
|---|---|---|
| 1 structure | JSON-schema, unique ids, `needs` exist, no self-edge, **cycle detection** (Kahn), `when` references an ancestor step and a field the skill returns | `REJECT_BAD_PLAN` |
| 2 per step | skill registered; args (missing / unknown / type / enum / range, defaults applied); step permissions ⊆ plan-declared; plan-declared ⊆ device-granted; safety class vs policy and `confirm` | `UNKNOWN_SKILL`, `BAD_ARGS`, `PERMISSION`, `SAFETY_CLASS` |
| 3 resources | for every unordered step pair, every same-named claim pair where either is `exclusive` | `REJECT_RESOURCE_CONFLICT` |
| 4 timing | WCET bound vs `deadline_ms`: longest path, sum, or Graham bound depending on the executor's worker count (see Timing model); on the device WCET and resource masks are re-derived from the skill table | `REJECT_DEADLINE_INFEASIBLE` |

### Resource conflicts

Resource names are resolved from the args first (`out_{channel}` → `out_warn_lamp`), so two
pulses on different channels never conflict. Two steps are *ordered* if one is a transitive
ancestor of the other (ancestor sets are built in topological order; on the device they are
`uint32_t` bit-masks). For every unordered pair, an exclusive claim against any claim of the
same resource is a conflict. Pairs whose `when` conditions are provably complementary
(same `ref` and `value`, opposite operators: `>`/`<=`, `<`/`>=`, `==`/`!=`) are
*mutually exclusive branches* and are skipped — in the demo mission `warn` (shared
`power_domain`) and `rest` (exclusive `power_domain`) are such a pair.

Design choice: **reject, don't serialize**. Because every claim is declared up front there is
no hold-and-wait, so deadlock cannot arise at run time; conflicts are an authoring error to be
fixed with `needs`, not something the scheduler silently papers over (which would invalidate
the timing analysis).

### Timing model

`wcet(step) = ceil(eval(contract.wcet_ms, resolved_args))`, evaluated by the Python reference
on the host and by a generated C function per skill on the device (`wcet_<skill>(args)` in
`skill_table.gen.c`; integer arithmetic with Python floor-division / modulo semantics, so the
two agree exactly - this is fuzz-tested, §10). **Every conditional branch is counted**
(conservative). The plan bound depends on how many steps the executor may run at once
(`policy.workers`; the device uses `CONFIG_PLANOSZ_EXEC_WORKERS`, default 2):

| workers | bound |
|---|---|
| 0 / unbounded (host virtual-time executor) | critical path `cp` |
| 1 | sum of all step WCETs |
| N > 1 | `cp` if the largest set of steps that can ever be runnable together, ω, is ≤ N; otherwise Graham's list-scheduling bound `min(sum, cp + ceil((sum - cp) / N))` |

ω is the maximum clique of the "may run concurrently" graph (no ancestor relation, not
mutually exclusive branches), computed with Bron-Kerbosch in both Python and C. A plain
critical-path bound would be **unsound** for a fixed pool (three ready steps, two workers), which
is why the pool size is part of the model. Demo numbers: `demo.parallel_sense` 1180 ms
(workers 0/2/3) vs 1550 ms (workers 1); `demo.parallel_overlap` 230 / 430 / 630 ms for workers 3 / 2 / 1.

The bound is only as good as the contract numbers - they are declared budgets, not measured or
statically proven. The executor enforces them at run time: each step has a watchdog on its
**device-derived** WCET (a step that exceeds it is `EXEC_TIMEOUT`), and the whole run has a
deadline watchdog. Skills are cooperative (they cannot be preempted mid-sample), so a watchdog
fires at the next opportunity and the run drains in-flight skills before `run_end`. Real-world
lesson recorded in the code: on `native_sim` with the default 10 ms tick, `k_msleep(5)` takes 20 ms;
the app therefore sets a 1 ms tick and `actuator.pulse` carries a +2 ms/pulse quantisation margin.

## 6. IR, the trust boundary and the wire format

`planoszc compile` turns an admitted plan into IR v1 (`runtime/include/planosz/ir.h`): steps in
topological order; per step `skill_idx`, `deps_mask`, `confirm`, resolved `args[4]` (enum =
index, float = milli) and `when_step/op/value_milli`; plus host-computed `wcet_ms` and
`excl/shared/mutex_mask` that the device **ignores**. Only a `when` on the producer's primary
(first) numeric output is representable; anything else is a compile error rather than a
silent device/host divergence.

Three ways to get IR to the device:

1. `--emit c`: a `static const` header compiled into the image (the CI demo plans).
2. `--emit uart`: wire format v1 (`runtime/src/ingress/ir_wire.c`, `planoszc/wire.py`) as paste
   lines for the shell: `planosz load begin` / `planosz load data <base64>` / `planosz load end`.
   Layout: `'P' 'Z'`, version, flags (0), perm u32, deadline u32, n_steps, plan id, then per step
   id, skill, deps, confirm, when_*, args, the (advisory) host WCET; trailing CRC-16/CCITT-FALSE;
   at most 2048 bytes. Bad magic, CRC, version, flags, truncation, trailing bytes, oversize or
   unsafe identifiers are `REJECT_UNSUPPORTED_IR`, with a `load` record in the trace, and leave the
   ephemeral upload slot empty. `planosz run uploaded` then runs the normal admission - an uploaded
   plan is never trusted more than an embedded one. Masks are not on the wire at all.
3. `--emit json`: for tooling.

`compile --force` emits IR for host-rejected plans and `--forge understate-wcet,strip-masks`
falsifies the advisory fields, so the device decision can be demonstrated and tested: the
`deadline_forged` / `conflict_forged` plans in the native_sim demo, the C tamper tests and the
CI upload test all prove a lying IR is still rejected (with `ir_wcet_mismatch` reported).

IR v1 is not CBOR.

## 7. Execution

### Host executor (`planoszc/executor.py`)

Event-driven, virtual-time DAG scheduler. At each instant every step whose dependencies are
done (or skipped) starts, up to `max_workers` at once (`None` = unbounded). A step with a false
`when` is skipped and counts as done. Durations come from `typical_ms` (or an injected value).
The resource manager locks claims and would fail loudly if admission ever let a conflict
through. A step error aborts the run, in-flight steps get `step_cancel` / `EXEC_CANCELLED`,
not-yet-started steps are reported in `not_started`. A deadline watchdog aborts with
`EXEC_TIMEOUT` if virtual time reaches `deadline_ms`.

### Device executor (`runtime/src/scheduler/executor.c`)

A coordinator (the calling thread) owns the ready set; N worker threads (`osal` threads: Zephyr
threads on device, pthreads under the gcc host tests) execute ready steps. One mutex guards all
scheduler state and trace lines are emitted while holding it with the timestamp read inside it, so
`seq` order is consistent with `t_ms`. For each ready step: evaluate `when` (skip -> `step_skip`
and counts as done) -> acquire the device-derived resource claims all-or-nothing (a runtime
guard: admission already proved no conflict, a violation is `EXEC_SKILL_FAIL "resource busy"`) ->
dispatch to a worker -> measure -> per-step WCET watchdog -> release. The first failure or a
watchdog expiry emits `step_cancel` for in-flight steps immediately, stops dispatch, drains the
workers and ends with `run_end FAIL`. `PLANOSZ_EXECUTOR_PARALLEL` + `CONFIG_PLANOSZ_EXEC_WORKERS`
select the model that admission uses; the run records `peak_concurrency`. `native_sim` runs
Zephyr threads on simulated time, so overlap there is *scheduling* overlap (shown in the trace:
`volt` and `cool` both start at 0 ms and the three-sweep plan finishes in ~256 ms instead of
~384 ms); physical multi-core timing needs real hardware.

## 8. Trace and replay

NDJSON, stable key order (`seq`, `type`, `t_ms`, ...). Types: `admit`, `load`, `run_start`,
`step_start`, `step_end`, `step_skip`, `step_error`, `step_cancel`, `deadline_miss`, `run_end`.
`step_start` carries the step's `wcet_ms`, `needs` and `claims` (`name:exclusive|shared`), the
device `admit` / `run_start` carry `workers`, and a device `admit` adds `ir_wcet_mismatch` when
the IR's WCET disagreed with the table. A `load` record (upload accepted / REJECT_UNSUPPORTED_IR)
stands alone with `seq` 0. Host: `planoszc run --format ndjson --trace file`; device: the same
events through `planosz_trace_set_sink()`.

`planoszc replay TRACE [--plan YAML ...] [--extract] [--require-overlap PLAN=N]
[--tolerance-ms N]` validates a trace (host golden, or a raw native_sim console log with
`--extract`) as *evidence*: `seq`/time monotonic, a REJECT has no side effects, every started
step ends exactly once, `dur_ms` matches timestamps and stays within the step's `wcet_ms`, no
two overlapping steps hold the same resource when either claim is exclusive, a step starts only
after all dependencies ended or were skipped, `when` branches agree with the observed producer
output, an OK run meets its deadline and its admitted bound, concurrency never exceeds the
admitted `workers`, and (with a plan) the admit decision equals a fresh admission of the YAML.
Exit 4 on any violation. CI replays the golden traces, the native_sim demo log, and the log of
the UART upload test. There is no on-device ring buffer; traces stream to the console.

## 9. Backends and HAL

`hal.h` is the whole seam: `sensor_read`, `output_set`, `delay_ms`, `idle_ms`, `sys_info`,
`uptime_ms`. Zephyr selects a backend through Kconfig (`PLANOSZ_BACKEND_NATIVE_SIM` /
`PLANOSZ_BACKEND_ESP32`, the latter default on ESP32 SoCs). `tests/host/c/test_runtime.c`
provides a third HAL (real-time, thread-safe, with fault-injection knobs) and the pthread OSAL so the C core, including the parallel executor, runs under plain `gcc`.

* `backends/native_sim`: simulated sensors, console-printing outputs, `k_msleep`.
* `backends/esp32`: `gpio_dt_spec` outputs from devicetree aliases `warn-lamp`, `cooling-fan`,
  `buzzer` (overlay in `apps/planosz_node/boards/`); sensors simulated (TODO ADC/I2C);
  idle is `k_msleep` (TODO PM).

## 10. Build and test layout

* Zephyr module: `zephyr/module.yml` + root `CMakeLists.txt` / `Kconfig`; library `planosz`.
* App: `apps/planosz_node` (adds the repo as an extra module itself). Overlays: `demo.conf`
  (autodemo, exit), `shell_stdio.conf` (shell on stdin/stdout for pipes and CI).
* Workspace: `west.yml` (T2 topology) or `tools/zephyr_setup.sh` (repo-local `.zephyr-ws`).
* Tests (`tests/host`, 114): admission per reject code; executor and golden traces; CLI;
  IDL/expr safety; codegen freshness; error-code sync across Python / C / docs;
  `test_c_runtime.py` builds the real runtime with gcc (pthread OSAL, real-time HAL so
  parallel steps genuinely overlap) and runs `tests/host/c/test_runtime.c` (admission vs
  Python, tampered IR, device derivation, resource table, watchdogs, failure paths, upload and
  corruption cases); `test_wcet_agreement.py` drives the same sources through `oracle.c` for
  every skill and arg boundary, 900 random DAGs x 4 worker counts (C vs Python code, WCET,
  critical path, omega) and forged-vs-honest IR equivalence; `test_replay.py` mutates golden
  traces to prove each replay rule fires; `test_wire_and_codegen.py` covers CRC/round-trip/
  corruption, `--emit uart`, `--forge`, worker bounds.
* CI (`.github/workflows/ci.yml`): host tests on Python 3.10 and 3.12 + golden replay + a
  tampered-trace check; a Zephyr job builds the `native_sim` demo, asserts ACCEPT/REJECT output
  and the forged-IR rejects, replays the device log, builds the stdio-shell image and runs
  `tools/ci/native_sim_shell.sh` (upload a good, a corrupt and two forged plans over the shell).

## 11. Spec vs code

| Master spec item | State |
|---|---|
| Contracts with resources/WCET/power/safety/permissions | done (`typical_ms` added for simulation) |
| Admission: schema, permission, resource conflict, WCET vs deadline | done, host + device; device recomputes WCET + masks |
| Extra code `REJECT_BAD_PLAN` | added (malformed plan, cycles); documented in `docs/error_codes.md` |
| DAG scheduling with parallel execution | done: host virtual time; device worker-pool executor (Zephyr threads), admission bound by worker count |
| Conditions | `when` on a dependency's primary numeric output; branches counted conservatively in WCET |
| IR / bytecode | C-struct IR v1 + a versioned, CRC'd wire encoding over the shell; CBOR not implemented |
| Codegen: validators, registry table, docs | registry table, arg ranges, per-skill WCET functions, resource templates done; doc fragments not generated |
| Trace / replay / fault injection | trace, golden, fault injection and `planoszc replay` done; on-device ring buffer not done |
| Skills `led.blink/led.set/gpio.write/power.sleep` | replaced by `actuator.pulse` / `power.idle` / `actuator.contactor` for a vehicle-like demo domain |
| `west planosz skill new` | not implemented; `west planosz` forwards to `planoszc` |
| zbus between ingress/admission/executor | not used; direct calls |
| Twister tests on native_sim | not set up; native_sim is exercised by the CI job and `tools/ci/native_sim_shell.sh` |
| ESP32 backend | cross-builds; GPIO real; sensors simulated; **never flashed** (no board) |
