"""Deterministic host executor: virtual-time DAG scheduler with resource locks.

Time is *simulated* (no sleeping), so runs are reproducible and CI-friendly. Steps whose
dependencies are satisfied start at the same virtual instant (parallel fork); a step with
a false `when` condition is skipped and counts as complete for its dependents.
"""

from __future__ import annotations

import heapq
import operator
from dataclasses import dataclass
from typing import Any

from . import errors as E
from .admission import Admission, StepInfo
from .backend_fake import FakeBackend, SkillFailure
from .trace import Trace

OPS = {
    ">": operator.gt,
    ">=": operator.ge,
    "<": operator.lt,
    "<=": operator.le,
    "==": operator.eq,
    "!=": operator.ne,
}


@dataclass
class RunResult:
    status: str  # OK | FAIL
    code: str
    total_ms: float
    outputs: dict[str, dict[str, Any]]


class ResourceManager:
    """Runtime lock table. Admission should make violations impossible; this proves it."""

    def __init__(self) -> None:
        self.held: dict[str, tuple[str, set[str]]] = {}

    def acquire(self, step: str, claims: list[tuple[str, str]]) -> str | None:
        for name, mode in claims:
            cur = self.held.get(name)
            if cur and ("exclusive" in (mode, cur[0])):
                return f"resource '{name}' busy (held by {sorted(cur[1])})"
        for name, mode in claims:
            cur = self.held.setdefault(name, (mode, set()))
            cur[1].add(step)
            if mode == "exclusive":
                self.held[name] = ("exclusive", cur[1])
        return None

    def release(self, step: str) -> None:
        for name in list(self.held):
            self.held[name][1].discard(step)
            if not self.held[name][1]:
                del self.held[name]


def run(adm: Admission, backend: FakeBackend, trace: Trace) -> RunResult:
    assert adm.accepted, "executor must only receive admitted plans"
    steps: dict[str, StepInfo] = {s.id: s for s in adm.steps}
    deadline = adm.deadline_ms or 0
    t = 0.0
    done: set[str] = set()
    started: set[str] = set()
    outputs: dict[str, dict[str, Any]] = {}
    running: list[tuple[float, int, str, float, str | None]] = []  # (end, idx, id, start, fail_code)
    locks = ResourceManager()

    trace.emit({"type": "run_start", "t_ms": 0, "plan": adm.plan_id, "deadline_ms": deadline, "steps": len(steps)})

    def abort(code: str, now: float, failing: str | None) -> RunResult:
        for _, _, sid, _, _ in sorted(running):
            if sid != failing:
                trace.emit({"type": "step_cancel", "t_ms": now, "step": sid, "code": E.EXEC_CANCELLED})
                locks.release(sid)
        pending = len(set(steps) - started - done)  # never started and not skipped
        trace.emit(
            {"type": "run_end", "t_ms": now, "plan": adm.plan_id, "status": "FAIL", "code": code,
             "total_ms": now, "not_started": pending}
        )
        return RunResult("FAIL", code, now, outputs)

    while len(done) < len(steps):
        progressed = True
        while progressed:
            progressed = False
            for st in adm.steps:
                if st.id in started or st.id in done or not all(n in done for n in st.needs):
                    continue
                if st.when:
                    ref_step, ref_field = st.when["ref"].split(".")
                    val = outputs[ref_step][ref_field]
                    ok = OPS[st.when["op"]](val, st.when["value"])
                    if not ok:
                        trace.emit({"type": "step_skip", "t_ms": t, "step": st.id,
                                    "cond": f"{st.when['ref']} {st.when['op']} {st.when['value']}",
                                    "observed": val})
                        done.add(st.id)
                        progressed = True
                        continue
                busy = locks.acquire(st.id, st.claims)
                if busy:
                    trace.emit({"type": "step_error", "t_ms": t, "step": st.id,
                                "code": E.EXEC_SKILL_FAIL, "detail": busy})
                    return abort(E.EXEC_SKILL_FAIL, t, st.id)
                started.add(st.id)
                trace.emit({"type": "step_start", "t_ms": t, "step": st.id, "action": st.action,
                            "args": st.args})
                try:
                    backend.check_available(st.contract)
                except SkillFailure as sf:
                    locks.release(st.id)
                    trace.emit({"type": "step_error", "t_ms": t, "step": st.id, "code": sf.code,
                                "detail": sf.detail})
                    return abort(sf.code, t, st.id)
                dur = backend.duration_ms(st.contract, st.args)
                fail = None
                if dur > st.wcet_ms:
                    dur, fail = float(st.wcet_ms), E.EXEC_TIMEOUT
                heapq.heappush(running, (t + dur, st.index, st.id, t, fail))
                progressed = True
        if len(done) >= len(steps):
            break
        if not running:  # pragma: no cover - impossible for an acyclic admitted DAG
            return abort(E.EXEC_CANCELLED, t, None)
        end, _, sid, start, fail = running[0]
        if end > deadline:
            t = float(deadline)
            trace.emit({"type": "deadline_miss", "t_ms": t, "deadline_ms": deadline,
                        "code": E.EXEC_TIMEOUT})
            return abort(E.EXEC_TIMEOUT, t, None)
        heapq.heappop(running)
        t = end
        st = steps[sid]
        locks.release(sid)
        if fail:
            trace.emit({"type": "step_error", "t_ms": t, "step": sid, "code": fail,
                        "detail": f"exceeded contract wcet_ms={st.wcet_ms}"})
            return abort(fail, t, sid)
        try:
            out = backend.run_skill(st.contract, st.args, t)
        except SkillFailure as sf:
            trace.emit({"type": "step_error", "t_ms": t, "step": sid, "code": sf.code,
                        "detail": sf.detail})
            return abort(sf.code, t, sid)
        outputs[sid] = out
        done.add(sid)
        trace.emit({"type": "step_end", "t_ms": t, "step": sid, "status": "OK", "dur_ms": t - start,
                    "out": out})

    trace.emit({"type": "run_end", "t_ms": t, "plan": adm.plan_id, "status": "OK", "code": E.OK,
                "total_ms": t})
    return RunResult("OK", E.OK, t, outputs)
