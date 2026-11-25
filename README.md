# PlanOS-Z

**A job-pack gatekeeper for ESP32 / Zephyr: it rejects unsafe or impossible multi-step jobs
*before* they run, then executes and traces the ones it accepts.**

PlanOS-Z is a contract-driven job-pack runtime for [Zephyr RTOS](https://zephyrproject.org).
Every capability ("skill") publishes a contract — arguments, resources, worst-case time,
permissions, safety class. A *job pack* (a small DAG of skill calls with a deadline) is
checked against those contracts and is either **ACCEPTED** or **REJECTED with a stable reason
code** — before a single pin moves. Accepted jobs run on a dependency-aware executor and
emit an NDJSON trace.

The domain is inspired by diagnostic "job packs" in automotive ECUs, but this is an
independent open project: the demo channels (`pack_voltage`, `coolant_proxy`, …) are fake,
and nothing here depends on any vendor's ECU, protocol or code.

```
 job pack (YAML)        skill contracts (IDL)
       │                       │
       ▼                       ▼
  ┌─────────────────────────────────────┐   host: planoszc (Python)
  │ check · admit · run · compile       │   - works today, no hardware, no Zephyr
  └───────────────┬─────────────────────┘
                  │ IR (C header / JSON)           codegen: IDL → C registry table
                  ▼
  ┌─────────────────────────────────────┐   device: Zephyr runtime (C)
  │ admission → executor → NDJSON trace │   - re-checks structure/args/perms/safety
  │ registry · resources · skills · HAL │   - WCET/resource masks still come from host IR (v1)
  └───────────────┬─────────────────────┘
                  ▼
        backends/native_sim   backends/esp32
```

## The problem

Embedded devices usually get behaviour in one of three weak ways: hard-coded firmware loops
(every change is a reflash), ad-hoc shell/MQTT commands (no timing guarantees, unsafe
composition), or "AI drives the GPIO" demos (no systems discipline at all). Anything that
accepts *jobs* from outside — a diagnostic tool, a gateway, a planner — needs the questions
answered **up front**: is this a known capability? are the arguments legal? is it allowed? do
two steps fight over the same hardware? can it possibly finish in time? PlanOS-Z answers them
at admission, with codes a machine can act on, and traces everything afterwards.

## Zephyr in one paragraph

Zephyr is an open-source (Apache-2.0, Linux Foundation) real-time OS for microcontrollers.
You describe hardware in **devicetree**, select software features in **Kconfig**, build with
CMake driven by the **west** meta-tool, and get a preemptive priority-based kernel
(threads, `k_msleep`, semaphores, message queues), a driver model, a shell, logging and
hundreds of supported boards. Its **`native_sim`** target compiles the same application
into a Linux process with simulated time — so the whole runtime is testable in CI with no
board. Third-party code ships as **modules**; this repository *is* a Zephyr module
(`zephyr/module.yml`).

## ESP32 target

The named MCU target is the **ESP32** (Xtensa LX6, `esp32_devkitc/esp32/procpu` in Zephyr
4.x). The runtime core never mentions a board; `backends/esp32/` is the only ESP32-specific
code (real GPIO outputs via devicetree aliases; sensors are simulated until hardware is
wired). **Status: cross-builds cleanly with the Zephyr SDK Xtensa toolchain; not yet
flashed on hardware** — see [docs/esp32_port.md](docs/esp32_port.md) and [ROADMAP.md](ROADMAP.md).

## Quickstart: `planoszc` (host, works now)

```bash
python3 -m venv .venv && . .venv/bin/activate
pip install -e "tools/planoszc[dev]"      # or: make install
python -m pytest tests/host -q            # 56 tests, ~1 s (or: make test)

planoszc skills                           # the 7 registered contracts
planoszc check examples/missions/01_parallel_sense_accept.yaml
planoszc admit examples/missions/02_deadline_reject.yaml
planoszc run   examples/missions/01_parallel_sense_accept.yaml
```

| Command | What it does | Exit |
|---|---|---|
| `planoszc check PLAN` | static analysis: contracts, deps, resources, per-step WCET table | 0 ok / 2 rejected |
| `planoszc admit PLAN` | admission gate with device policy (`--grant`, `--allow-hazardous`) | 0 / 2 |
| `planoszc run PLAN` | admit, then execute on the deterministic fake backend; `--format ndjson`, `--trace FILE`, `--sensor N=V`, `--inject FAULT` | 0 / 2 / 3 |
| `planoszc compile PLAN --emit c` | compile to IR (C header or JSON) for the device | 0 |
| `planoszc skills` | list contracts | 0 |
| `planoszc codegen` | IDL → C registry table (`runtime/src/registry/skill_table.gen.c`) | 0 |

Also available as `west planosz ...` inside a workspace that uses this repo's `west.yml`.

## Demo: ACCEPT

```text
$ planoszc admit examples/missions/01_parallel_sense_accept.yaml
ACCEPTED  demo.parallel_sense
  steps=5 estimated_wcet_ms=1180 deadline_ms=2000
  critical_path=cool->warn->report

$ planoszc run examples/missions/01_parallel_sense_accept.yaml
ACCEPTED  demo.parallel_sense
  steps=5 estimated_wcet_ms=1180 deadline_ms=2000
  critical_path=cool->warn->report
TRACE
  [     0 ms] start  volt     sensor.read(sensor=pack_voltage, samples=1)
  [     0 ms] start  cool     sensor.read(sensor=coolant_proxy, samples=4)     <- parallel fork
  [    15 ms] end    volt     ok dur=15ms out={"value":48.2,"unit":"V"}
  [    30 ms] end    cool     ok dur=30ms out={"value":31.2,"unit":"degC"}      <- join
  [    30 ms] start  warn     actuator.pulse(channel=warn_lamp, count=5, ...)
  [    30 ms] skip   rest     cond false: cool.value <= 30 (observed 31.2)
  [  1030 ms] end    warn     ok dur=1000ms out={"pulses":5}
  [  1030 ms] start  report   diag.report(scope=summary)
  [  1040 ms] end    report   ok dur=10ms out={"records":1}
  [  1040 ms] run_end OK code=OK total=1040ms
RESULT: ok  total_ms=1040 deadline_ms=2000
```

Add `--format ndjson` for the machine stream (one JSON object per line), `--sensor coolant_proxy=22`
to take the other branch, or `--inject sensor_fail:coolant_proxy` for fault injection
(`EXEC_SKILL_FAIL`, in-flight steps `EXEC_CANCELLED`, exit code 3).

## Demo: REJECT with reason codes

```text
$ planoszc admit examples/missions/02_deadline_reject.yaml
REJECTED  REJECT_DEADLINE_INFEASIBLE
  plan:   demo.deadline_reject
  detail: estimated_wcet_ms=2594 deadline_ms=2000 critical_path=cool->warn->report
$ echo $?
2

$ planoszc admit examples/missions/03_resource_conflict_reject.yaml
REJECTED  REJECT_RESOURCE_CONFLICT
  plan:   demo.resource_conflict
  detail: steps 'lamp_a' (exclusive) and 'lamp_b' (exclusive) may run concurrently and both claim
          'out_warn_lamp' with an exclusive claim; add `needs` to order them
```

Machine-readable form (`--format json`):

```json
{"seq":0,"t_ms":0,"type":"admit","plan":"demo.deadline_reject","status":"REJECT","code":"REJECT_DEADLINE_INFEASIBLE","wcet_ms":2594,"deadline_ms":2000,"detail":"..."}
```

Nothing executes on a reject: `planoszc run` on those files prints the REJECT and exits 2.
Other rejects covered by tests: unknown skill, bad args, permissions (declared *and* granted),
`HAZARDOUS` / unconfirmed skills, dependency cycles, malformed plans — all listed in
[docs/error_codes.md](docs/error_codes.md).

## Zephyr: the same decisions on the device

The C runtime re-implements admission over the compiled IR, so the device never has to
trust the host. `make zephyr-setup` (once) then:

```bash
make native-sim-demo     # builds apps/planosz_node for native_sim and runs it
make esp32-build         # cross-builds for esp32_devkitc/esp32/procpu
```

Real output from the Zephyr build (native_sim). The device executor is sequential, so the
device admits against the **sum** of WCETs (1550 ms, vs 1180 ms critical path on the host:
`planoszc admit --executor sequential` reproduces the device number):

```text
{"seq":0,"type":"admit","t_ms":0,"plan":"demo.parallel_sense","status":"ACCEPT","code":"OK","wcet_ms":1550,"deadline_ms":2000,"steps":5}
...
RESULT: OK
{"seq":0,"type":"admit","t_ms":0,"plan":"demo.deadline_reject","status":"REJECT","code":"REJECT_DEADLINE_INFEASIBLE","step":-1,"wcet_ms":2964,"deadline_ms":2000}
RESULT: REJECT_DEADLINE_INFEASIBLE
{"seq":0,"type":"admit","t_ms":0,"plan":"demo.resource_conflict","status":"REJECT","code":"REJECT_RESOURCE_CONFLICT","step":1,"wcet_ms":0,"deadline_ms":3000}
RESULT: REJECT_RESOURCE_CONFLICT
```

Interactive: `build/native_sim/zephyr/zephyr.exe` prints a PTY path; attach and run
`planosz plans`, `planosz admit conflict`, `planosz run accept`. Full bring-up, versions and
disk/time numbers: [docs/zephyr_setup.md](docs/zephyr_setup.md).

## How a job is judged

1. **Structure** – schema, unique ids, known dependencies, no cycles → `REJECT_BAD_PLAN`
2. **Per step** – skill exists, args within contract ranges, permissions declared *and*
   granted, safety class allowed → `UNKNOWN_SKILL / BAD_ARGS / PERMISSION / SAFETY_CLASS`
3. **Resources** – any two steps with no dependency path between them must not overlap on a
   resource if either claims it *exclusively* (complementary `when` branches are recognised
   as mutually exclusive) → `REJECT_RESOURCE_CONFLICT`
4. **Timing** – worst-case time from the contract WCETs, counting every conditional branch,
   must fit `deadline_ms` → `REJECT_DEADLINE_INFEASIBLE`. The model matches the executor:
   critical path when steps can overlap (host), sum of steps when they run one by one (device v0.1)

Details and limits: [ARCHITECTURE.md](ARCHITECTURE.md).

## Repository map

| Path | Contents |
|---|---|
| `idl/skills/*.yaml`, `idl/schemas/` | 7 skill contracts + JSON Schemas for contracts and plans |
| `tools/planoszc/` | Python package: IDL loader, admission, executor, compile, codegen, CLI |
| `tools/west_commands/` | `west planosz` extension |
| `runtime/` | C core: registry, admission, resources, executor, trace, ingress/shell |
| `skills/src/` | skill handlers (talk to the world only through the HAL) |
| `backends/native_sim`, `backends/esp32` | one HAL implementation each |
| `apps/planosz_node/` | Zephyr app image (shell + demo plans) |
| `examples/missions/` | the ACCEPT / REJECT / REJECT demo job packs |
| `traces/golden/` | golden NDJSON trace compared in CI |
| `tests/host/` | 56 pytest tests, incl. a gcc-built C unit test cross-checked against Python |
| `west.yml`, `zephyr/module.yml` | Zephyr workspace manifest / module definition |
| `docs/` | error codes, skill IDL, plan language, use case, ESP32 port, Zephyr setup, master spec |

## Docs

[ARCHITECTURE.md](ARCHITECTURE.md) · [ROADMAP.md](ROADMAP.md) ·
[docs/USE_CASE.md](docs/USE_CASE.md) ·
[docs/error_codes.md](docs/error_codes.md) · [docs/skill_idl.md](docs/skill_idl.md) ·
[docs/plan_language.md](docs/plan_language.md) · [docs/esp32_port.md](docs/esp32_port.md) ·
[docs/zephyr_setup.md](docs/zephyr_setup.md) · [docs/MASTER_SPEC.txt](docs/MASTER_SPEC.txt)

## Honest status

| Area | State |
|---|---|
| `planoszc` check/admit/run/compile, 7 contracts, NDJSON, fault injection | done, tested |
| C admission + resource table + executor + trace + registry codegen | done, host-tested with gcc and cross-checked against Python |
| Zephyr `native_sim` build + run, shell commands | builds and runs; same ACCEPT/REJECT decisions as the host |
| Executor parallelism on the device | **sequential in topological order**, so device admission uses the sum-of-WCET model; the host executor forks in parallel (critical-path model). Device DAG dispatch is roadmap phase 5 |
| ESP32 | cross-builds; outputs are real GPIO, sensors simulated; **not flashed/tested on hardware** |
| WCET | a declared contract budget, not a measured/proved bound |

## Add a skill (≈15 minutes)

1. Write `idl/skills/<name>.yaml` ([docs/skill_idl.md](docs/skill_idl.md)).
2. `make codegen` (regenerates the C registry table; a test fails if it is stale).
3. Add a `case` for it in `skills/src/skills.c` and a fake in `tools/planoszc/planoszc/backend_fake.py`.
4. Add a plan test in `tests/host/`. 5. `make test`.

## Resume bullet

> Built **PlanOS-Z**, a contract-driven job-pack runtime for Zephyr RTOS (ESP32 target): skill
> IDL → generated C registry; admission control that rejects unknown skills, bad arguments,
> permission/safety violations, exclusive-resource conflicts on dependency DAGs and
> deadline-infeasible jobs (critical-path WCET) before any side effect; DAG executor with
> NDJSON tracing and fault injection; host tool `planoszc` plus an on-device C re-validation
> cross-checked against it; 56 host tests, a golden trace, and a CI workflow (host tests + `native_sim` build and run).

## License

Apache-2.0 — see [LICENSE](LICENSE).
