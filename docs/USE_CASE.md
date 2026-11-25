# Use case: a job-pack gatekeeper on a small node

## Scenario

A small ESP32 node sits on a battery pack test bench (demo domain — fake channels
`pack_voltage`, `coolant_proxy`, a warning lamp, a fan, a buzzer). A laptop tool, a gateway or
a technician's script sends it **jobs**: "sample voltage and coolant temperature, flash the
warning lamp if it's hot, otherwise idle, then write a diagnostic summary — and be done within
2 s." (`examples/missions/01_parallel_sense_accept.yaml`)

## Without PlanOS-Z

| Approach | What goes wrong |
|---|---|
| Hard-coded firmware | every new job = new firmware, reflash, regression risk |
| Raw serial/MQTT commands (`PULSE lamp 12`) | commands compose with no checks: two jobs drive the same lamp, a 12-pulse blink silently misses the deadline, an unknown command fails halfway through |
| "Let the planner/AI drive GPIO" | impressive demo; nothing prevents an unsafe or impossible sequence |

## With PlanOS-Z

The device only runs a job after answering, in order:

1. Is the plan well-formed (no cycles, no dangling dependencies)?
2. Is every step a **registered skill** with **legal arguments**?
3. Does the plan **declare**, and the device **grant**, the permissions each skill needs?
   Is the skill's **safety class** allowed (hazardous contactor = no by default)?
4. Can any two steps that may run at the same time **fight over a resource**
   (exclusive lamp output, exclusive trace sink, exclusive power domain)?
5. Is the **worst-case time** (critical path, or a worker-count-aware bound on the device's thread pool) within the deadline?

Each "no" is a stable code (`REJECT_UNKNOWN_SKILL`, `REJECT_BAD_ARGS`, `REJECT_PERMISSION`,
`REJECT_SAFETY_CLASS`, `REJECT_RESOURCE_CONFLICT`, `REJECT_DEADLINE_INFEASIBLE`,
`REJECT_BAD_PLAN`) that the sender can act on automatically — no guessing from a log line.
Accepted jobs run with per-step timeout and a deadline watchdog; everything is traced as NDJSON,
so a failure on the bench can be reproduced on `native_sim`.

## The three shipped missions

| File | Result | Why |
|---|---|---|
| `01_parallel_sense_accept.yaml` | ACCEPT (wcet 1180 ms ≤ 2000 ms) | two sensor reads share `adc0`; `warn`/`rest` are complementary branches |
| `02_deadline_reject.yaml` | `REJECT_DEADLINE_INFEASIBLE` (2594 ms critical path > 2000 ms) | lamp asked to pulse 12×; contracts say it cannot fit |
| `03_resource_conflict_reject.yaml` | `REJECT_RESOURCE_CONFLICT` | two unordered pulses on `out_warn_lamp`; fix = `needs: [lamp_a]` |

## Where else the same shape applies

Any device that takes *jobs* from outside and touches hardware: a gateway that schedules
actuator sequences, a robot's behaviour executive, an industrial controller accepting recipe
steps, a diagnostic tool running multi-step service routines against an embedded node.
PlanOS-Z is deliberately small: the point is the **admission discipline** and the
**contract → codegen → runtime** path, not a particular product.

## What this is not

Not a certified safety system: WCET values are declared budgets (see
declared contract budgets, not measured or proved bounds), and
the sensors on ESP32 are simulated today.
