# Skill IDL (contracts)

One YAML file per skill in `idl/skills/`. Validated against
[`idl/schemas/contract.schema.json`](../idl/schemas/contract.schema.json) when loaded
(`planoszc skills` lists them; any error names the file).

```yaml
skill_id: actuator.pulse            # lower-case dotted id
version: 1
summary: Pulse a low-power output channel N times (lamp, fan, buzzer).
args:                               # order matters: it is the IR argument order (max 4)
  channel: { type: enum, values: [warn_lamp, cooling_fan, buzzer] }
  count:   { type: uint16, min: 1, max: 50 }
  on_ms:   { type: uint32, min: 1, max: 5000, default: 200 }
  off_ms:  { type: uint32, min: 1, max: 5000, default: 200 }
returns:
  pulses: { type: uint16 }
resources:
  - { name: "out_{channel}", mode: exclusive }   # {arg} is substituted from the step's args
  - { name: power_domain,    mode: shared }
wcet_ms: "count * (on_ms + off_ms + 2) + 20"     # worst-case budget, expression over args
typical_ms: "count * (on_ms + off_ms)"           # optional: what the simulator uses
power_class: ACTIVE_LOW             # ACTIVE_HIGH | ACTIVE_LOW | SLEEP
safety_class: SAFE                  # SAFE | REQUIRES_CONFIRM | HAZARDOUS
permissions: [ACTUATE]
```

## Fields

| Field | Meaning |
|---|---|
| `args.*.type` | `uint8 uint16 uint32 int32 float bool enum string`; `min`/`max` for numbers, `values` for enums, optional `default` (an arg with no default is required) |
| `returns` | output fields; a `when:` in a plan may only reference a field listed here |
| `resources` | what the skill claims while it runs; `exclusive` conflicts with any other claim of the same name, `shared` only with an exclusive one |
| `wcet_ms` | arithmetic expression over arg names. Allowed: numbers, strings, `+ - * / // %`, comparisons, `x if cond else y`, `min()`, `max()`. Parsed with an AST whitelist, never `eval` |
| `power_class`, `safety_class`, `permissions` | policy inputs; `HAZARDOUS` is rejected unless the device allows it, `REQUIRES_CONFIRM` needs `confirm: true` on the step |

## Current skills

| skill | resources | safety | permissions |
|---|---|---|---|
| `sensor.read` | `adc0`(sh), `power_domain`(sh) | SAFE | READ_SENSOR |
| `actuator.pulse` | `out_{channel}`(ex), `power_domain`(sh) | SAFE | ACTUATE |
| `wait` | – | SAFE | – |
| `power.idle` | `power_domain`(ex) | SAFE | POWER_CONTROL |
| `sys.info` | – | SAFE | – |
| `diag.report` | `trace_sink`(ex) | SAFE | DIAG |
| `actuator.contactor` | `contactor0`(ex), `power_domain`(sh) | **HAZARDOUS** | ACTUATE, HV_CONTROL |

## Adding a skill (checklist)

1. `idl/skills/<id>.yaml`.
2. `make codegen` → regenerates `runtime/include/planosz/skill_table.gen.h` and
   `runtime/src/registry/skill_table.gen.c` (indices are sorted by id, so existing indices can
   shift — regenerate any compiled demo plans with `make app-plans`).
3. Handler: a `case` in `skills/src/skills.c` (use the generated `PLANOSZ_SKILL_*` enum; args
   arrive in IDL order) and, if it needs new hardware, a HAL function in `hal.h` + both backends.
4. Host fake: `tools/planoszc/planoszc/backend_fake.py::run_skill`.
5. Tests in `tests/host/` (admission: good, bad args, conflict; executor if it has behaviour).
6. `make test`. The sync tests fail if generated code or error-code docs are stale.
