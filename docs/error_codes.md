# Error and reason codes

Every admission decision and every execution failure carries one **stable, machine-readable
code** plus a human-readable `detail`. Codes are the public API surface of PlanOS-Z: they are
the same in the Python tool (`planoszc/errors.py`), the C runtime
(`runtime/include/planosz/errors.h`, numeric values frozen) and this document. A host test
(`tests/host/test_idl_and_sync.py`) fails if any of the three drift apart.

Rule: **append only**. Never rename or renumber a code.

## Admission (before any side effect) - process exit code 2

| # | Code | Stage | Meaning | Typical fix |
|---|------|-------|---------|-------------|
| 0 | `OK` | - | Accepted / succeeded. | - |
| 1 | `REJECT_UNKNOWN_SKILL` | 2 per-step | A step's `action` is not in the device registry. | Fix the name or add the skill (IDL + codegen). |
| 2 | `REJECT_BAD_ARGS` | 2 per-step | Missing/unknown arg, wrong type, enum miss, or value outside the contract's range. | Fix the args; see `planoszc skills`. |
| 3 | `REJECT_PERMISSION` | 2 per-step | The skill needs a permission the plan did not declare, **or** the plan asks for one the device policy does not grant. | Declare it in `permissions:` / change device grants. |
| 4 | `REJECT_RESOURCE_CONFLICT` | 3 resources | Two steps with no ordering between them could run at the same time and one claims a resource exclusively. | Order them with `needs:` or use different resources. |
| 5 | `REJECT_DEADLINE_INFEASIBLE` | 4 timing | Worst-case time from the contracts (critical path, or sum of steps for a sequential executor) exceeds `deadline_ms`. | Shorten the job or relax the deadline. |
| 6 | `REJECT_UNSUPPORTED_IR` | device | IR version/format not understood by this runtime. Reserved for the device ingress; the host tool never emits it. | Recompile with a matching `planoszc`. |
| 7 | `REJECT_SAFETY_CLASS` | 2 per-step | `HAZARDOUS` skill while policy forbids it, or `REQUIRES_CONFIRM` skill without `confirm: true`. | Explicit confirmation / device policy. |
| 8 | `REJECT_BAD_PLAN` | 1 structure | Malformed plan: schema violation, duplicate id, unknown dependency, dependency cycle, bad `when`. | Fix the document. |

Stages run in order; the **first failing stage decides the primary code**. Later violations in
the same stage are listed as `also:` lines / in the `violations` array of the NDJSON event.

## Execution (after admission) - process exit code 3

| # | Code | Meaning |
|---|------|---------|
| 9 | `EXEC_TIMEOUT` | A step exceeded its contract WCET, or the plan exceeded its deadline (watchdog). |
| 10 | `EXEC_SKILL_FAIL` | A skill handler/backend reported failure (e.g. sensor read error, or a resource lock was unexpectedly busy). |
| 11 | `EXEC_CANCELLED` | Step cancelled because another step failed or the deadline watchdog fired. |
| 12 | `EXEC_BACKEND_UNAVAILABLE` | The backend does not implement / cannot reach the skill's hardware. |

## CLI exit codes

| Exit | Meaning |
|------|---------|
| 0 | accepted / run OK |
| 1 | usage or I/O error (bad file, bad `--inject`) |
| 2 | rejected at admission |
| 3 | admitted but execution failed |

## NDJSON examples

```json
{"seq":0,"type":"admit","t_ms":0,"plan":"demo.deadline_reject","status":"REJECT","code":"REJECT_DEADLINE_INFEASIBLE","wcet_ms":2594,"deadline_ms":2000,"detail":"estimated_wcet_ms=2594 deadline_ms=2000 critical_path=cool->warn->report","violations":[{"code":"REJECT_DEADLINE_INFEASIBLE","detail":"..."}]}
{"seq":9,"type":"step_error","t_ms":55,"step":"cool","code":"EXEC_SKILL_FAIL","detail":"sensor 'coolant_proxy' read failed (injected)"}
```
