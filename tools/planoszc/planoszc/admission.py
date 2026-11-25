"""Admission control: decide ACCEPT / REJECT for a plan *before any side effect*.

Stages (first failing stage decides the primary reason code):

  1. structure   REJECT_BAD_PLAN            schema, ids, deps, cycles, `when` refs
  2. per step    REJECT_UNKNOWN_SKILL / REJECT_BAD_ARGS / REJECT_PERMISSION /
                 REJECT_SAFETY_CLASS
  3. resources   REJECT_RESOURCE_CONFLICT   unordered steps with an exclusive overlap
  4. timing      REJECT_DEADLINE_INFEASIBLE critical-path WCET > deadline_ms

The same checks are re-implemented in C (runtime/src/admission) over a compiled IR so
the device never has to trust the host blindly.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import yaml
from jsonschema import Draft202012Validator

from . import errors as E
from .idl import Contract, Registry, load_plan_schema

COMPLEMENT = {">": "<=", "<=": ">", "<": ">=", ">=": "<", "==": "!=", "!=": "=="}


class PlanLoadError(Exception):
    pass


def load_plan(path: str | Path) -> dict:
    p = Path(path)
    try:
        with open(p, encoding="utf-8") as fh:
            doc = yaml.safe_load(fh)
    except (OSError, yaml.YAMLError) as exc:
        raise PlanLoadError(f"cannot read plan {p}: {exc}") from exc
    if not isinstance(doc, dict):
        raise PlanLoadError(f"{p}: plan must be a mapping/object")
    return doc


@dataclass
class Policy:
    """Device-side policy applied on top of the plan's own declarations."""

    granted_permissions: set[str] | None = None  # None = grant everything requested
    allow_hazardous: bool = False
    # Timing model must match the executor that will run the plan:
    #   False -> concurrent steps overlap: WCET = critical path (host executor)
    #   True  -> steps run one after another: WCET = sum of all steps (device executor v0.1)
    sequential_executor: bool = False


@dataclass
class Violation:
    code: str
    detail: str
    step: str | None = None

    def to_dict(self) -> dict:
        d: dict[str, Any] = {"code": self.code, "detail": self.detail}
        if self.step:
            d["step"] = self.step
        return d


@dataclass
class StepInfo:
    id: str
    action: str
    contract: Contract
    args: dict[str, Any]
    needs: list[str]
    confirm: bool
    when: dict | None
    wcet_ms: int
    typical_ms: float
    claims: list[tuple[str, str]]
    index: int = 0
    earliest_finish_ms: int = 0  # worst-case, along the critical path


@dataclass
class Admission:
    plan_id: str
    deadline_ms: int | None
    permissions: list[str] = field(default_factory=list)
    violations: list[Violation] = field(default_factory=list)
    steps: list[StepInfo] = field(default_factory=list)
    order: list[str] = field(default_factory=list)  # topological order of step ids
    wcet_ms: int | None = None
    critical_path: list[str] = field(default_factory=list)
    mutex: dict[str, set[str]] = field(default_factory=dict)

    @property
    def accepted(self) -> bool:
        return not self.violations

    @property
    def code(self) -> str:
        return self.violations[0].code if self.violations else E.OK

    @property
    def status(self) -> str:
        return "ACCEPT" if self.accepted else "REJECT"

    def step(self, sid: str) -> StepInfo:
        return next(s for s in self.steps if s.id == sid)

    def primary_detail(self) -> str:
        return self.violations[0].detail if self.violations else ""

    def to_event(self) -> dict:
        ev: dict[str, Any] = {
            "type": "admit",
            "plan": self.plan_id,
            "status": self.status,
            "code": self.code,
        }
        if self.wcet_ms is not None:
            ev["wcet_ms"] = self.wcet_ms
        if self.deadline_ms is not None:
            ev["deadline_ms"] = self.deadline_ms
        if self.violations:
            ev["detail"] = self.primary_detail()
            ev["violations"] = [v.to_dict() for v in self.violations]
        else:
            ev["steps"] = len(self.steps)
        return ev


# --------------------------------------------------------------------------- helpers
def _topo(ids: list[str], needs: dict[str, list[str]]) -> tuple[list[str], list[str] | None]:
    """Kahn's algorithm, stable w.r.t. file order. Returns (order, cycle_members|None)."""
    indeg = {i: len(needs[i]) for i in ids}
    children: dict[str, list[str]] = {i: [] for i in ids}
    for i in ids:
        for n in needs[i]:
            children[n].append(i)
    ready = [i for i in ids if indeg[i] == 0]
    order: list[str] = []
    while ready:
        cur = ready.pop(0)
        order.append(cur)
        for ch in children[cur]:
            indeg[ch] -= 1
            if indeg[ch] == 0:
                ready.append(ch)
    if len(order) != len(ids):
        return order, [i for i in ids if i not in order]
    return order, None


def _mutually_exclusive(a: StepInfo, b: StepInfo) -> bool:
    wa, wb = a.when, b.when
    if not wa or not wb:
        return False
    return (
        wa["ref"] == wb["ref"]
        and wa["value"] == wb["value"]
        and COMPLEMENT.get(wa["op"]) == wb["op"]
    )


# --------------------------------------------------------------------------- main entry
def admit(plan: dict, registry: Registry, policy: Policy | None = None) -> Admission:
    policy = policy or Policy()
    plan_id = plan.get("plan_id") if isinstance(plan.get("plan_id"), str) else "<unnamed>"
    deadline = plan.get("deadline_ms") if isinstance(plan.get("deadline_ms"), int) else None
    adm = Admission(plan_id=plan_id, deadline_ms=deadline)

    # ---- stage 1: structure ---------------------------------------------------
    validator = Draft202012Validator(load_plan_schema(registry.idl_dir))
    schema_errs = sorted(validator.iter_errors(plan), key=lambda e: [str(p) for p in e.path])
    if schema_errs:
        for e in schema_errs:
            where = "/".join(str(p) for p in e.path) or "<plan>"
            adm.violations.append(Violation(E.REJECT_BAD_PLAN, f"{where}: {e.message}"))
        return adm
    adm.permissions = list(plan["permissions"])

    raw_steps = plan["steps"]
    ids = [s["id"] for s in raw_steps]
    dupes = sorted({i for i in ids if ids.count(i) > 1})
    for d in dupes:
        adm.violations.append(Violation(E.REJECT_BAD_PLAN, f"duplicate step id '{d}'", d))
    needs: dict[str, list[str]] = {}
    for s in raw_steps:
        needs[s["id"]] = list(s.get("needs", []))
        for n in needs[s["id"]]:
            if n == s["id"]:
                adm.violations.append(Violation(E.REJECT_BAD_PLAN, f"step '{n}' depends on itself", n))
            elif n not in ids:
                adm.violations.append(
                    Violation(E.REJECT_BAD_PLAN, f"step '{s['id']}' needs unknown step '{n}'", s["id"])
                )
    if adm.violations:
        return adm
    order, cyc = _topo(ids, needs)
    if cyc:
        adm.violations.append(
            Violation(E.REJECT_BAD_PLAN, f"dependency cycle among steps {cyc}", cyc[0])
        )
        return adm
    adm.order = order

    # ancestors (transitive needs) for `when` validation and ordering
    anc: dict[str, set[str]] = {}
    for sid in order:
        a: set[str] = set()
        for n in needs[sid]:
            a |= {n} | anc[n]
        anc[sid] = a
    for s in raw_steps:
        w = s.get("when")
        if w:
            ref_step = w["ref"].split(".")[0]
            if ref_step not in anc[s["id"]]:
                adm.violations.append(
                    Violation(
                        E.REJECT_BAD_PLAN,
                        f"step '{s['id']}' has `when` on '{ref_step}' which is not a (transitive) dependency",
                        s["id"],
                    )
                )
    if adm.violations:
        return adm

    # ---- stage 2: per-step contract checks -----------------------------------
    infos: dict[str, StepInfo] = {}
    granted = policy.granted_permissions
    for perm in adm.permissions:
        if granted is not None and perm not in granted:
            adm.violations.append(
                Violation(E.REJECT_PERMISSION, f"plan requests permission {perm} which the device does not grant")
            )
    for s in raw_steps:
        sid, action = s["id"], s["action"]
        c = registry.get(action)
        if c is None:
            adm.violations.append(
                Violation(E.REJECT_UNKNOWN_SKILL, f"step '{sid}': skill '{action}' is not registered", sid)
            )
            continue
        resolved, arg_errs = c.resolve_args(s.get("args"))
        for ae in arg_errs:
            adm.violations.append(Violation(E.REJECT_BAD_ARGS, f"step '{sid}' ({action}): {ae}", sid))
        missing = [p for p in c.permissions if p not in adm.permissions]
        if missing:
            adm.violations.append(
                Violation(
                    E.REJECT_PERMISSION,
                    f"step '{sid}' ({action}) needs permission {', '.join(missing)} not declared by plan",
                    sid,
                )
            )
        if c.safety_class == "HAZARDOUS" and not policy.allow_hazardous:
            adm.violations.append(
                Violation(
                    E.REJECT_SAFETY_CLASS,
                    f"step '{sid}' ({action}) is HAZARDOUS and device policy forbids it",
                    sid,
                )
            )
        elif c.safety_class == "REQUIRES_CONFIRM" and not s.get("confirm", False):
            adm.violations.append(
                Violation(
                    E.REJECT_SAFETY_CLASS,
                    f"step '{sid}' ({action}) requires explicit `confirm: true`",
                    sid,
                )
            )
        w = s.get("when")
        if w:
            ref_step, ref_field = w["ref"].split(".")
            ref_c = registry.get(next(r["action"] for r in raw_steps if r["id"] == ref_step))
            if ref_c is not None and ref_field not in ref_c.returns:
                adm.violations.append(
                    Violation(
                        E.REJECT_BAD_PLAN,
                        f"step '{sid}': `when` field '{w['ref']}' is not returned by {ref_c.skill_id}",
                        sid,
                    )
                )
        if not arg_errs:
            infos[sid] = StepInfo(
                id=sid,
                action=action,
                contract=c,
                args=resolved,
                needs=needs[sid],
                confirm=bool(s.get("confirm", False)),
                when=w,
                wcet_ms=math.ceil(c.wcet_ms(resolved)),
                typical_ms=c.typical_ms(resolved),
                claims=c.claims(resolved),
            )
    if adm.violations:
        return adm
    adm.steps = [infos[i] for i in ids]
    for idx, st in enumerate(adm.steps):
        st.index = idx

    # ---- stage 3: resource conflicts on the DAG ------------------------------
    for i, a in enumerate(adm.steps):
        for b in adm.steps[i + 1 :]:
            if a.id in anc[b.id] or b.id in anc[a.id]:
                continue  # ordered by dependencies -> can never overlap
            if _mutually_exclusive(a, b):
                adm.mutex.setdefault(a.id, set()).add(b.id)
                adm.mutex.setdefault(b.id, set()).add(a.id)
                continue
            for ra, ma in a.claims:
                for rb, mb in b.claims:
                    if ra == rb and "exclusive" in (ma, mb):
                        adm.violations.append(
                            Violation(
                                E.REJECT_RESOURCE_CONFLICT,
                                f"steps '{a.id}' ({ma}) and '{b.id}' ({mb}) may run concurrently "
                                f"and both claim '{ra}' with an exclusive claim; add `needs` to order them",
                                a.id,
                            )
                        )
    if adm.violations:
        return adm

    # ---- stage 4: timing (critical-path WCET, all conditional branches counted) --
    pred: dict[str, str | None] = {}
    for sid in order:
        st = infos[sid]
        best, best_p = 0, None
        for n in st.needs:
            if best_p is None or infos[n].earliest_finish_ms > best:
                best, best_p = infos[n].earliest_finish_ms, n
        st.earliest_finish_ms = best + st.wcet_ms
        pred[sid] = best_p
    if policy.sequential_executor:
        # No overlap: every step may execute one after another, so the sum is the bound.
        run_t = 0
        for sid in order:
            run_t += infos[sid].wcet_ms
            infos[sid].earliest_finish_ms = run_t
        adm.wcet_ms = run_t
        adm.critical_path = list(order)
        mode = " executor=sequential"
    else:
        end = max(order, key=lambda i: infos[i].earliest_finish_ms)
        adm.wcet_ms = infos[end].earliest_finish_ms
        path = []
        cur: str | None = end
        while cur:
            path.append(cur)
            cur = pred[cur]
        adm.critical_path = list(reversed(path))
        mode = ""
    if adm.wcet_ms > plan["deadline_ms"]:
        adm.violations.append(
            Violation(
                E.REJECT_DEADLINE_INFEASIBLE,
                f"estimated_wcet_ms={adm.wcet_ms} deadline_ms={plan['deadline_ms']} "
                f"critical_path={'->'.join(adm.critical_path)}{mode}",
            )
        )
    return adm
