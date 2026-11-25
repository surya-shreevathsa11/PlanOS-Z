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
* **Partial trust boundary (honest v1).** The host compiles a plan to IR; the device
  re-checks structure, args, permissions, and safety class against its own registry.
  Per-step WCET and resource bit-masks are still taken from the IR today — a malicious
  host can under-state them. Recomputing those from the on-device skill table is on the
  roadmap; do not claim full “never trusts the host” until that lands.
* **Core knows nothing about boards.** A narrow HAL (6 functions) is the only seam.
* **Everything observable.** One NDJSON event per admit/start/end/skip/error.
* **Deterministic first.** Host executor uses virtual time; `native_sim` runs in simulated
  time; golden traces are compared in CI.

## 2. Component map

```
idl/skills/*.yaml ──► planoszc.idl (load + validate vs contract.schema.json)
                          │                          │
                          │                          └─► planoszc.codegen ─► runtime/src/registry/skill_table.gen.c
 plan.yaml ─► planoszc.admission.admit() ──ACCEPT──► planoszc.executor.run()  (host, virtual time)
                          │                                   │ events
                          │ ACCEPT/REJECT                     ▼
                          │                          planoszc.trace (NDJSON)
                          └─► planoszc.compile ──► IR (JSON / C header)
                                                        │
 ┌──────────────────────────────── Zephyr app (apps/planosz_node) ──────────────────────────────┐
 │ runtime/src/ingress   register named IR plans, shell: planosz status|skills|plans|admit|run  │
 │        │                                                                                      │
 │        ▼                                                                                      │
 │ runtime/src/admission  planosz_admit(IR, policy) → code      (pure C, host-testable)         │
 │        │ ACCEPT                                                                               │
 │        ▼                                                                                      │
 │ runtime/src/scheduler  planosz_exec_run(): order, conditions, timeout, deadline watchdog     │
 │   ├─ runtime/src/resource   exclusive/shared lock masks                                      │
 │   ├─ skills/src/skills.c    one dispatch on the generated skill index                        │
 │   └─ runtime/src/trace      NDJSON through a pluggable sink                                  │
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
| 4 timing | WCET bound vs `deadline_ms`: longest path over the DAG (parallel executor) or sum of all steps (sequential executor) | `REJECT_DEADLINE_INFEASIBLE` |

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

`wcet(step) = ceil(eval(contract.wcet_ms, resolved_args))`. **Every conditional branch is
counted** (conservative). The bound must match how the executor will actually run the plan,
so there are two models, selected by policy (`Policy.sequential_executor` /
`planosz_policy.sequential_executor` / `planoszc admit --executor`):

* *parallel* (host executor, virtual time): finish of a step = max(finish of its dependencies)
  + wcet; plan WCET = max finish = **critical path**. Valid only if concurrent steps really
  overlap (one core per ready step, no interference).
* *sequential* (device executor v0.1): steps run one after another, so plan WCET = **sum of all
  step WCETs**. This is what `planosz_admit` uses on the device
  (`PLANOSZ_EXECUTOR_PARALLEL 0` in `executor.h`; flip it only together with a parallel
  dispatcher). For the demo job: 1180 ms parallel vs 1550 ms sequential. The bound is only as good as the contract
numbers — they are declared budgets, not measured or statically proven. The simulator
enforces them at run time (a step that exceeds its WCET is `EXEC_TIMEOUT`), which is how bad
contracts get discovered. Real-world lesson recorded in the code: on `native_sim` with the
default 10 ms tick, `k_msleep(5)` takes 20 ms; the app therefore sets a 1 ms tick and the
`actuator.pulse` contract carries a +2 ms/pulse quantisation margin.

## 6. IR and the trust boundary

`planoszc compile` turns an admitted plan into IR v1 (`runtime/include/planosz/ir.h`):
steps in topological order; per step `skill_idx`, `wcet_ms`, `deps_mask`, `excl_mask`,
`shared_mask`, `mutex_mask`, `confirm`, resolved `args[4]` (enum = index, float = milli),
`when_step/op/value_milli`. `--emit c` writes a `static const` C header (that is how the
Zephyr app embeds plans); `--emit json` is for tooling. The device runs `planosz_admit()` on
it and trusts nothing: bad indices, out-of-range args, forward dependency edges, missing
permissions all reject. (`compile --force` emits IR for host-rejected plans so the device
decision can be shown and tested; the `tests/host/test_c_runtime.py` test checks that C and
Python agree on all three missions.)

IR v1 is a C struct, not CBOR; a compact wire encoding is roadmap.

## 7. Execution

### Host executor (`planoszc/executor.py`)

Event-driven, virtual-time DAG scheduler. At each instant every step whose dependencies are
done (or skipped) starts — a real parallel fork/join. A step with a false `when` is skipped
and counts as done. Durations come from `typical_ms` (or an injected value). The resource
manager locks claims and would fail loudly if admission ever let a conflict through. Failure
handling: a step error (`EXEC_SKILL_FAIL`, `EXEC_TIMEOUT`, `EXEC_BACKEND_UNAVAILABLE`) aborts
the run, in-flight steps get `step_cancel` / `EXEC_CANCELLED`, not-yet-started steps are
reported in `not_started`. A deadline watchdog aborts with `EXEC_TIMEOUT` if virtual time
reaches `deadline_ms` (unreachable for contract-respecting steps after sound admission, kept
as defence in depth and unit-tested by shrinking the deadline post-admission).

### Device executor (`runtime/src/scheduler/executor.c`)

Runs the IR in **topological order, sequentially** (v0.1): condition check → resource
acquire → skill dispatch → measure → per-step WCET check → release → trace. Deadline is
checked before every step. Because it is sequential, the device admits with the sum-of-WCET
model (§5); a job that fits only by overlapping steps is rejected on the device even though the
host admits it. Parallel dispatch (threads/work queue per ready step) is roadmap phase 5; the
IR already carries the dependency/resource masks it needs, and the resource-conflict rule
is applied conservatively today so enabling it will not admit anything new.

## 8. Trace

NDJSON, stable key order (`seq`, `type`, `t_ms`, …). Types: `admit`, `run_start`,
`step_start`, `step_end`, `step_skip`, `step_error`, `step_cancel`, `deadline_miss`,
`run_end`. Host: `planoszc run --format ndjson --trace file`. Device: the same event names
through `planosz_trace_set_sink()` (console in the app). `traces/golden/` holds the golden
host trace compared in `tests/host/test_executor.py`. Replay tooling and a ring buffer export
are roadmap phase 6.

## 9. Backends and HAL

`hal.h` is the whole seam: `sensor_read`, `output_set`, `delay_ms`, `idle_ms`, `sys_info`,
`uptime_ms`. Zephyr selects a backend through Kconfig (`PLANOSZ_BACKEND_NATIVE_SIM` /
`PLANOSZ_BACKEND_ESP32`, the latter default on ESP32 SoCs). `tests/host/c/test_runtime.c`
provides a third HAL with a virtual clock so the C core runs under plain `gcc`.

* `backends/native_sim`: simulated sensors, console-printing outputs, `k_msleep`.
* `backends/esp32`: `gpio_dt_spec` outputs from devicetree aliases `warn-lamp`, `cooling-fan`,
  `buzzer` (overlay in `apps/planosz_node/boards/`); sensors simulated (TODO ADC/I2C);
  idle is `k_msleep` (TODO PM).

## 10. Build and test layout

* Zephyr module: `zephyr/module.yml` + root `CMakeLists.txt` / `Kconfig`; library `planosz`.
* App: `apps/planosz_node` (adds the repo as an extra module itself).
* Workspace: `west.yml` (T2 topology) or `tools/zephyr_setup.sh` (repo-local `.zephyr-ws`).
* Tests (`tests/host`, 56): admission per reject code, boundary and mutual-exclusion cases;
  executor (fork/join timing, branches, faults, watchdog, determinism, golden trace); CLI
  (exit codes, NDJSON validity); IDL/expr safety; codegen freshness; error-code sync across
  Python / C / docs; a gcc-built C test run against the same missions as Python.
* CI (`.github/workflows/ci.yml`): host tests on Python 3.10 and 3.12; a Zephyr job builds
  and runs the `native_sim` app in a T2 workspace and asserts the ACCEPT/REJECT output.

## 11. Spec vs code

| Master spec item | State |
|---|---|
| Contracts with resources/WCET/power/safety/permissions | done (`typical_ms` added for simulation) |
| Admission: schema, permission, resource conflict, WCET vs deadline | done, host + device |
| Extra code `REJECT_BAD_PLAN` | added (malformed plan, cycles); documented in `docs/error_codes.md` |
| DAG scheduling with parallel execution | host: yes (virtual time, critical-path model); device: sequential topological (sum-of-WCET model) |
| Conditions | `when` on a dependency's output; branches counted conservatively in WCET |
| IR / bytecode | C-struct IR v1 (header/JSON); CBOR not implemented |
| Codegen: validators, registry table, docs | registry table + arg ranges done; doc fragments not generated |
| Trace / replay / fault injection | trace + golden + fault injection done; replay harness and device ring buffer not done |
| Skills `led.blink/led.set/gpio.write/power.sleep` | replaced by `actuator.pulse` / `power.idle` / `actuator.contactor` for a vehicle-like demo domain |
| `west planosz skill new` | not implemented; `west planosz` forwards to `planoszc` |
| zbus between ingress/admission/executor | not used; direct calls |
| Twister tests on native_sim | not set up; native_sim app is exercised by the CI job instead |
| ESP32 backend | builds; GPIO real; sensors simulated; no hardware run |
