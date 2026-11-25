"""Trace replay: validate an NDJSON run (host executor or device log) against the PlanOS-Z
execution contract. A trace is *evidence*; this checks that the evidence is internally
consistent and, given the plan YAML, consistent with the contracts too.

Rules (each violation carries a stable rule name):

  seq            events numbered 0..n-1 within a run; t_ms never goes backwards
  admission      first event is `admit`; a REJECT run contains nothing else (no side effects)
  lifecycle      run_start after ACCEPT, exactly one run_end last; every started step ends exactly
                 once (end/error/cancel); skipped steps never start; an OK run accounts for every step
  wcet           dur_ms matches the timestamps and never exceeds the step's wcet_ms
  resources      steps overlapping in time never hold the same resource when either claim is exclusive
  causality      a step starts only after every dependency ended OK or was skipped
  condition      `when` branches taken/skipped agree with the observed producer output  (needs plan)
  deadline       OK runs finish within deadline_ms and within the admitted WCET bound
  concurrency    never more steps in flight than the admission's `workers`; optional minimum
                 overlap proof (--require-overlap)
  plan           admit decision / wcet / step contract fields equal a fresh admission of the plan YAML

Used by `planoszc replay`, tests/host/test_replay.py and CI (golden host trace + native_sim log).
"""

from __future__ import annotations

import json
import operator
from dataclasses import dataclass, field
from typing import Any, Iterable

from . import errors as E
from .admission import Admission, Policy, admit

OPS = {">": operator.gt, ">=": operator.ge, "<": operator.lt, "<=": operator.le, "==": operator.eq,
       "!=": operator.ne}
TERMINAL = ("step_end", "step_error", "step_cancel")


@dataclass
class Violation:
    rule: str
    detail: str
    seq: int | None = None

    def __str__(self) -> str:
        where = f" (seq {self.seq})" if self.seq is not None else ""
        return f"[{self.rule}]{where} {self.detail}"


@dataclass
class RunReport:
    plan_id: str = "<none>"
    kind: str = "run"  # run | load
    status: str = ""  # ACCEPT | REJECT | OK | FAIL
    code: str = ""
    events: int = 0
    total_ms: float | None = None
    peak_concurrency: int = 0
    steps_ok: int = 0
    steps_skipped: int = 0
    violations: list[Violation] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not self.violations

    def summary(self) -> str:
        mark = "ok  " if self.ok else "FAIL"
        if self.kind == "load":
            return f"{mark} load   plan={self.plan_id} {self.status} {self.code}"
        tail = ""
        if self.status in ("OK", "FAIL") and self.total_ms is not None:
            tail = (f" total_ms={self.total_ms:g} steps_ok={self.steps_ok} skipped={self.steps_skipped}"
                    f" peak_concurrency={self.peak_concurrency}")
        return f"{mark} run    plan={self.plan_id} result={self.status or '?'} code={self.code or '?'}{tail}"


class ReplayError(Exception):
    pass


# ---------------------------------------------------------------------------- parsing
def parse_lines(lines: Iterable[str], extract: bool = False) -> list[dict]:
    """NDJSON -> events. With `extract`, anything that is not a trace event (boot banner,
    `[sim]` console lines, shell prompts) is skipped, as in a raw device console log."""
    events: list[dict] = []
    for n, raw in enumerate(lines, 1):
        text = raw.strip()
        if not text:
            continue
        if extract:
            start = text.find('{"seq":')
            if start < 0:
                continue
            text = text[start:]
        try:
            ev = json.loads(text)
        except json.JSONDecodeError as exc:
            if extract:
                continue
            raise ReplayError(f"line {n}: not valid JSON ({exc.msg})") from exc
        if not isinstance(ev, dict) or "type" not in ev or "seq" not in ev:
            if extract:
                continue
            raise ReplayError(f"line {n}: not a trace event (needs 'seq' and 'type')")
        events.append(ev)
    return events


def split_runs(events: list[dict]) -> list[list[dict]]:
    runs: list[list[dict]] = []
    for ev in events:
        if ev["seq"] == 0 or not runs:
            runs.append([])
        runs[-1].append(ev)
    return runs


# ---------------------------------------------------------------------------- checking
def _claims_conflict(a: list[str], b: list[str]) -> str | None:
    ma = {c.rsplit(":", 1)[0]: c.rsplit(":", 1)[1] for c in a}
    for c in b:
        name, mode = c.rsplit(":", 1)
        if name in ma and "exclusive" in (mode, ma[name]):
            return name
    return None


def _num(v: Any) -> float:
    return float(v)


def check_run(events: list[dict], plan: dict | None = None, registry=None, *,
              tolerance_ms: float = 0.0, require_overlap: int | None = None) -> RunReport:
    rep = RunReport(events=len(events))
    v = rep.violations

    def bad(rule: str, detail: str, ev: dict | None = None) -> None:
        v.append(Violation(rule, detail, None if ev is None else ev.get("seq")))

    first = events[0]
    if first["type"] == "load":
        return _check_load(events, rep)
    rep.plan_id = str(first.get("plan", "<none>"))
    rep.status, rep.code = str(first.get("status", "")), str(first.get("code", ""))

    # -- seq / time --
    last_t = None
    for i, ev in enumerate(events):
        if ev["seq"] != i:
            bad("seq", f"expected seq {i}, got {ev['seq']}", ev)
        t = ev.get("t_ms")
        if t is None:
            bad("seq", f"event '{ev['type']}' has no t_ms", ev)
            continue
        if last_t is not None and t < last_t:
            bad("seq", f"t_ms went backwards ({last_t} -> {t})", ev)
        last_t = t if last_t is None else max(last_t, t)

    # -- admission --
    if first["type"] != "admit":
        bad("admission", f"first event is '{first['type']}', must be 'admit'", first)
        return rep
    if rep.status == "REJECT":
        if len(events) != 1:
            bad("admission", f"REJECTed plan produced {len(events) - 1} further event(s) - side effects "
                             f"before/after a reject", events[1])
        if rep.code in ("OK", ""):
            bad("admission", "REJECT without a reason code", first)
        _check_plan_admission(first, plan, registry, bad)
        return rep
    if rep.status != "ACCEPT" or rep.code != E.OK:
        bad("admission", f"admit status/code '{rep.status}/{rep.code}' invalid", first)
        return rep
    deadline = first.get("deadline_ms")
    admit_wcet = first.get("wcet_ms")
    workers = first.get("workers") or None
    if deadline is not None and admit_wcet is not None and admit_wcet > deadline:
        bad("admission", f"ACCEPT with wcet_ms={admit_wcet} > deadline_ms={deadline}", first)
    adm = _check_plan_admission(first, plan, registry, bad)

    # -- lifecycle --
    if len(events) < 2 or events[1]["type"] != "run_start":
        bad("lifecycle", "ACCEPT must be followed by run_start", events[1] if len(events) > 1 else first)
        return rep
    run_start = events[1]
    n_steps = run_start.get("steps")
    if events[-1]["type"] != "run_end":
        bad("lifecycle", "trace does not end with run_end", events[-1])
        return rep
    run_end = events[-1]
    for ev in events[2:-1]:
        if ev["type"] in ("run_start", "run_end", "admit"):
            bad("lifecycle", f"unexpected '{ev['type']}' inside a run", ev)
    rep.total_ms = _num(run_end.get("total_ms", 0))
    rep.status = str(run_end.get("status", ""))
    rep.code = str(run_end.get("code", ""))
    if run_start.get("deadline_ms") != deadline:
        bad("lifecycle", "run_start deadline_ms differs from admit", run_start)

    starts: dict[str, dict] = {}
    ends: dict[str, dict] = {}
    skips: dict[str, dict] = {}
    for ev in events[2:-1]:
        t = ev["type"]
        sid = ev.get("step")
        if t == "step_start":
            if sid in starts:
                bad("lifecycle", f"step '{sid}' started twice", ev)
            if sid in skips:
                bad("lifecycle", f"step '{sid}' started after being skipped", ev)
            starts[sid] = ev
        elif t in TERMINAL:
            if sid not in starts:
                bad("lifecycle", f"{t} for step '{sid}' that never started", ev)
            elif sid in ends:
                bad("lifecycle", f"step '{sid}' ended twice", ev)
            else:
                ends[sid] = ev
        elif t == "step_skip":
            if sid in starts:
                bad("lifecycle", f"step '{sid}' skipped after starting", ev)
            if sid in skips:
                bad("lifecycle", f"step '{sid}' skipped twice", ev)
            skips[sid] = ev
        elif t != "deadline_miss":
            bad("lifecycle", f"unknown event type '{t}'", ev)
    ok_ends = {s: e for s, e in ends.items() if e["type"] == "step_end"}
    rep.steps_ok, rep.steps_skipped = len(ok_ends), len(skips)
    if rep.status == "OK":
        if rep.code != E.OK:
            bad("lifecycle", f"run_end OK with code {rep.code}", run_end)
        if n_steps is not None and len(ok_ends) + len(skips) != n_steps:
            bad("lifecycle", f"OK run accounts for {len(ok_ends) + len(skips)} of {n_steps} steps", run_end)
        for sid in starts:
            if sid not in ok_ends:
                bad("lifecycle", f"step '{sid}' started but did not complete in an OK run", starts[sid])
    elif rep.status == "FAIL":
        if rep.code in ("", E.OK):
            bad("lifecycle", "FAIL run without a failure code", run_end)
        for sid in starts:
            if sid not in ends:
                bad("lifecycle", f"step '{sid}' started but has no end/error/cancel in a FAIL run", starts[sid])
    else:
        bad("lifecycle", f"run_end status '{rep.status}' invalid", run_end)

    # -- per-step wcet / duration --
    iv: dict[str, tuple[float, float]] = {}
    for sid, st in starts.items():
        t0 = _num(st["t_ms"])
        en = ends.get(sid)
        if en is None:
            continue
        t1 = _num(en["t_ms"])
        iv[sid] = (t0, t1)
        if t1 < t0:
            bad("wcet", f"step '{sid}' ends before it starts", en)
        if en["type"] == "step_end":
            dur = _num(en.get("dur_ms", t1 - t0))
            if abs(dur - (t1 - t0)) > 1 + tolerance_ms:
                bad("wcet", f"step '{sid}' dur_ms={dur:g} but timestamps span {t1 - t0:g}", en)
            wcet = st.get("wcet_ms")
            if wcet is not None and dur > wcet + tolerance_ms:
                bad("wcet", f"step '{sid}' ran {dur:g} ms, over its WCET {wcet} ms", en)

    # -- resources: overlapping intervals must not conflict --
    claims = {sid: list(st.get("claims", [])) for sid, st in starts.items()}
    sids = sorted(iv, key=lambda s: iv[s])
    for i, a in enumerate(sids):
        for b in sids[i + 1 :]:
            if iv[b][0] >= iv[a][1]:
                continue  # sorted by start: b begins after a ended
            name = _claims_conflict(claims[a], claims[b])
            if name:
                bad("resources", f"steps '{a}' and '{b}' overlap in time [{iv[a][0]:g},{iv[a][1]:g}] / "
                                 f"[{iv[b][0]:g},{iv[b][1]:g}] and conflict on '{name}'", starts[b])

    # -- causality --
    for sid, st in starts.items():
        for dep in st.get("needs", []):
            done = ok_ends.get(dep) or skips.get(dep)
            if done is None:
                bad("causality", f"step '{sid}' started but dependency '{dep}' never completed OK", st)
            elif _num(st["t_ms"]) < _num(done["t_ms"]):
                bad("causality", f"step '{sid}' started at {st['t_ms']} before dependency '{dep}' "
                                 f"finished at {done['t_ms']}", st)
    for sid, sk in skips.items():
        for dep in _plan_needs(plan, sid):
            if dep not in ok_ends and dep not in skips:
                bad("causality", f"step '{sid}' skipped but dependency '{dep}' never completed", sk)

    # -- concurrency --
    points = sorted([(a, 1) for a, _ in iv.values()] + [(b, -1) for _, b in iv.values()],
                    key=lambda p: (p[0], p[1]))  # an end at t frees before a start at t
    cur = peak = 0
    for _, d in points:
        cur += d
        peak = max(peak, cur)
    rep.peak_concurrency = peak
    if workers is not None and peak > workers:
        bad("concurrency", f"{peak} steps in flight but the admission declared workers={workers}", run_end)
    if require_overlap is not None and peak < require_overlap:
        bad("concurrency", f"required overlap of {require_overlap} steps, observed peak {peak}", run_end)

    # -- deadline / admitted bound --
    if rep.status == "OK" and deadline is not None:
        if rep.total_ms > deadline + tolerance_ms:
            bad("deadline", f"OK run took {rep.total_ms:g} ms, deadline is {deadline} ms", run_end)
        if admit_wcet is not None and rep.total_ms > admit_wcet + tolerance_ms:
            bad("deadline", f"run took {rep.total_ms:g} ms, over the admitted WCET bound {admit_wcet} ms", run_end)
    for ev in events:
        if ev["type"] == "deadline_miss":
            if deadline is not None and ev["t_ms"] < deadline - tolerance_ms:
                bad("deadline", f"deadline_miss at {ev['t_ms']} ms, before deadline {deadline}", ev)
            if rep.status != "FAIL" or rep.code != E.EXEC_TIMEOUT:
                bad("deadline", "deadline_miss but run_end is not FAIL/EXEC_TIMEOUT", ev)

    # -- plan-aware checks --
    if adm is not None and adm.accepted:
        _check_against_plan(adm, starts, skips, ok_ends, bad)
    return rep


def _plan_needs(plan: dict | None, sid: str) -> list[str]:
    if not plan:
        return []
    return next((list(s.get("needs", [])) for s in plan["steps"] if s["id"] == sid), [])


def _check_load(events: list[dict], rep: RunReport) -> RunReport:
    rep.kind = "load"
    ev = events[0]
    rep.plan_id = str(ev.get("plan", "<none>"))
    rep.status, rep.code = str(ev.get("status", "")), str(ev.get("code", ""))
    if len(events) != 1:
        rep.violations.append(Violation("lifecycle", "a load record must stand alone", events[1].get("seq")))
    if rep.status not in ("OK", "REJECT"):
        rep.violations.append(Violation("lifecycle", f"load status '{rep.status}' invalid", ev.get("seq")))
    if (rep.status == "OK") != (rep.code == E.OK):
        rep.violations.append(Violation("lifecycle", "load status and code disagree", ev.get("seq")))
    if rep.status == "REJECT" and rep.code != E.REJECT_UNSUPPORTED_IR:
        rep.violations.append(Violation("lifecycle", f"load rejected with {rep.code}, expected "
                                                      f"{E.REJECT_UNSUPPORTED_IR}", ev.get("seq")))
    return rep


def _check_plan_admission(first: dict, plan: dict | None, registry, bad) -> Admission | None:
    if plan is None or registry is None:
        return None
    adm = admit(plan, registry, Policy(workers=first.get("workers") or None))
    if adm.status != first.get("status") or adm.code != first.get("code"):
        bad("plan", f"trace says {first.get('status')}/{first.get('code')} but the plan admits as "
                    f"{adm.status}/{adm.code}", first)
    elif adm.wcet_ms is not None and first.get("wcet_ms") is not None and adm.wcet_ms != first["wcet_ms"]:
        bad("plan", f"trace wcet_ms={first['wcet_ms']} but contracts give {adm.wcet_ms}", first)
    return adm


def _check_against_plan(adm: Admission, starts, skips, ok_ends, bad) -> None:
    by_id = {s.id: s for s in adm.steps}
    for sid, st in starts.items():
        info = by_id.get(sid)
        if info is None:
            bad("plan", f"trace runs step '{sid}' which is not in the plan", st)
            continue
        if st.get("action") != info.action:
            bad("plan", f"step '{sid}' action {st.get('action')} != plan {info.action}", st)
        if "args" in st and st["args"] != info.args:
            bad("plan", f"step '{sid}' args {st['args']} != plan {info.args}", st)
        if "wcet_ms" in st and st["wcet_ms"] != info.wcet_ms:
            bad("plan", f"step '{sid}' wcet_ms={st['wcet_ms']} but its contract says {info.wcet_ms}", st)
        if "needs" in st and sorted(st["needs"]) != sorted(info.needs):
            bad("plan", f"step '{sid}' needs {st['needs']} != plan {info.needs}", st)
        if "claims" in st:
            want = sorted(f"{n}:{m}" for n, m in info.claims)
            if sorted(st["claims"]) != want:
                bad("plan", f"step '{sid}' claims {sorted(st['claims'])} != contract {want}", st)
        if info.when and not _cond_ok(info, ok_ends, expect=True):
            bad("condition", f"step '{sid}' ran although `{info.when['ref']} {info.when['op']} "
                             f"{info.when['value']}` is false for the observed output", st)
    for sid, sk in skips.items():
        info = by_id.get(sid)
        if info is None or info.when is None:
            bad("condition", f"step '{sid}' was skipped without a `when` condition", sk)
        elif not _cond_ok(info, ok_ends, expect=False):
            bad("condition", f"step '{sid}' skipped although its condition holds for the observed output", sk)


def _observed(info, ok_ends) -> float | None:
    ref_step, ref_field = info.when["ref"].split(".")
    end = ok_ends.get(ref_step)
    if end is None:
        return None
    if "out" in end:
        val = end["out"].get(ref_field)
        return float(val) if isinstance(val, (int, float)) else None
    if "out_milli" in end:
        return end["out_milli"] / 1000.0
    return None


def _cond_ok(info, ok_ends, expect: bool) -> bool:
    """Does the recorded condition outcome (`expect`: step ran / was skipped) agree with the
    producer's observed output? A skipped producer reads as 0 on the device."""
    value = info.when["value"]
    if isinstance(value, (str, bool)):
        return True
    obs = _observed(info, ok_ends)
    if obs is None:
        if info.when["ref"].split(".")[0] in ok_ends:
            return True  # non-numeric output: cannot judge
        obs = 0.0
    return OPS[info.when["op"]](obs, float(value)) == expect


# ---------------------------------------------------------------------------- driver
def replay(events: list[dict], plans: dict[str, dict] | None = None, registry=None, *,
           tolerance_ms: float = 0.0, require_overlap: dict[str, int] | None = None) -> list[RunReport]:
    plans = plans or {}
    require_overlap = require_overlap or {}
    reports = []
    for run in split_runs(events):
        pid = run[0].get("plan")
        reports.append(check_run(run, plans.get(pid) if pid else None, registry, tolerance_ms=tolerance_ms,
                                 require_overlap=require_overlap.get(pid) if pid else None))
    # every required overlap must have been exercised by an executed run
    seen = {r.plan_id for r in reports if r.kind == "run" and r.status in ("OK", "FAIL")}
    for pid, n in require_overlap.items():
        if pid not in seen:
            rep = RunReport(plan_id=pid)
            rep.violations.append(Violation("concurrency", f"required overlap {n} for plan '{pid}' but no "
                                                           f"executed run of it is in the trace"))
            reports.append(rep)
    return reports
