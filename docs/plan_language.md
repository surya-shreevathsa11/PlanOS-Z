# Plan language (v1)

A **plan** (job pack / mission) is a YAML or JSON document, validated against
[`idl/schemas/plan.schema.json`](../idl/schemas/plan.schema.json).

```yaml
plan_id: demo.parallel_sense          # [A-Za-z0-9_.-]+
description: optional text
deadline_ms: 2000                     # whole job must finish within this
permissions: [READ_SENSOR, ACTUATE, POWER_CONTROL, DIAG]   # what the plan asks for
steps:                                # 1..32 steps
  - id: volt                          # [A-Za-z][A-Za-z0-9_]*, unique
    action: sensor.read               # a registered skill
    args: { sensor: pack_voltage }    # checked against the contract; defaults applied
  - id: cool
    action: sensor.read
    args: { sensor: coolant_proxy, samples: 4 }
  - id: warn
    action: actuator.pulse
    args: { channel: warn_lamp, count: 5, on_ms: 100, off_ms: 100 }
    needs: [volt, cool]               # dependencies = the DAG edges
    when: { ref: cool.value, op: ">", value: 30 }
  - id: rest
    action: power.idle
    args: { duration_ms: 300 }
    needs: [volt, cool]
    when: { ref: cool.value, op: "<=", value: 30 }
  - id: report
    action: diag.report
    needs: [warn, rest]
```

## Semantics

* **DAG, not a list.** `needs` are the edges. Steps with no dependency path between them are
  *concurrent*; the host executor really starts them at the same instant. Cycles are
  `REJECT_BAD_PLAN`.
* **Skipped steps count as done.** `report` above runs after whichever branch ran.
* **`when`** compares an output field of a *dependency* (`step.field`, the field must be in
  that skill's `returns`) with a literal; operators `> >= < <= == !=`. If false, the step is
  skipped (`step_skip` event). `ref` must be a (transitive) dependency, else `REJECT_BAD_PLAN`.
* **Admission treats all branches as possibly taken**: WCET counts both `warn` and `rest`.
  Two branches whose conditions are provably complementary (same `ref` and `value`, opposite
  operators) are known to be mutually exclusive, so they do not count as a resource conflict.
  Any other pair of unordered steps is assumed able to overlap.
* **Permissions** — the plan lists what it needs; every step's skill permissions must be
  in that list (`REJECT_PERMISSION`), and the list must be within the device grant
  (`planoszc admit --grant A,B`).
* **Safety** — `confirm: true` on a step satisfies a `REQUIRES_CONFIRM` skill; `HAZARDOUS`
  skills need device policy (`--allow-hazardous`), a plan cannot grant itself that.
* **Deadline** is compared with the worst-case time from the contract WCETs
  (`estimated_wcet_ms ≤ deadline_ms` is accepted; equality passes). Default model: *critical
  path* (unbounded workers, host executor). `--workers N` models an N-thread executor (the
  device default is 2): critical path if at most N steps can ever run at once, otherwise
  Graham's bound; `--workers 1` (alias `--executor sequential`) uses the *sum* of all steps.
  `when` must reference the producing step's primary (first) numeric return field to compile
  to the device IR.

## Not in v1

Loops, dynamic step creation, data flow between steps beyond `when`, per-step deadlines,
priorities. They would each need an admission story first.
