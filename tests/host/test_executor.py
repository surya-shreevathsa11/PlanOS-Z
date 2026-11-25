"""Fake executor: deterministic virtual-time DAG execution + NDJSON events + fault injection."""

import json

from planoszc import errors as E
from planoszc.admission import admit
from planoszc.backend_fake import FakeBackend, FaultInjection
from planoszc.executor import run
from planoszc.trace import Trace

from conftest import ROOT


def go(plan, registry, sensors=None, faults=None):
    adm = admit(plan, registry)
    tr = Trace()
    res = run(adm, FakeBackend(sensors, FaultInjection.parse(faults or [])), tr)
    return res, tr.events


def types(events):
    return [(e["type"], e.get("step")) for e in events]


def test_hot_path_runs_warn_skips_rest(registry, accept_plan):
    res, ev = go(accept_plan, registry)
    assert res.status == "OK" and res.total_ms == 1040
    t = types(ev)
    assert ("step_skip", "rest") in t and ("step_end", "warn") in t


def test_cool_path_runs_rest_skips_warn(registry, accept_plan):
    res, ev = go(accept_plan, registry, sensors={"coolant_proxy": 22.0})
    assert res.status == "OK"
    t = types(ev)
    assert ("step_skip", "warn") in t and ("step_end", "rest") in t
    assert res.total_ms == 30 + 305 + 10


def test_parallel_fork_starts_at_same_instant(registry, accept_plan):
    _, ev = go(accept_plan, registry)
    starts = {e["step"]: e["t_ms"] for e in ev if e["type"] == "step_start"}
    assert starts["volt"] == starts["cool"] == 0
    assert starts["warn"] == 30  # joins on the slower of the two sensors


def test_join_waits_for_all_deps(registry, accept_plan):
    _, ev = go(accept_plan, registry, faults=["slow:sensor.read=55"])
    ends = {e["step"]: e["t_ms"] for e in ev if e["type"] == "step_end"}
    assert ends["volt"] == ends["cool"] == 55
    starts = {e["step"]: e["t_ms"] for e in ev if e["type"] == "step_start"}
    assert starts["warn"] == 55


def test_trace_is_deterministic(registry, accept_plan):
    import copy

    _, a = go(copy.deepcopy(accept_plan), registry)
    _, b = go(copy.deepcopy(accept_plan), registry)
    assert a == b
    assert [e["seq"] for e in a] == list(range(len(a)))


def test_sensor_failure_aborts_and_cancels(registry, accept_plan):
    res, ev = go(accept_plan, registry, faults=["sensor_fail:coolant_proxy"])
    assert res.status == "FAIL" and res.code == E.EXEC_SKILL_FAIL
    assert ev[-1]["type"] == "run_end" and ev[-1]["code"] == E.EXEC_SKILL_FAIL
    assert not any(e["type"] == "step_start" and e["step"] == "warn" for e in ev)


def test_step_timeout_when_exceeding_contract(registry, accept_plan):
    res, ev = go(accept_plan, registry, faults=["slow:sensor.read=500"])
    assert res.code == E.EXEC_TIMEOUT
    err = [e for e in ev if e["type"] == "step_error"][0]
    assert "wcet_ms" in err["detail"]


def test_deadline_watchdog_aborts_and_cancels(registry, base_plan):
    # A sound admission makes a deadline miss impossible; simulate "reality disagrees with the
    # contract" by shrinking the deadline after admission. The watchdog must still stop the job.
    adm = admit(base_plan, registry)
    adm.deadline_ms = 100
    tr = Trace()
    res = run(adm, FakeBackend(), tr)
    assert res.code == E.EXEC_TIMEOUT and res.total_ms == 100
    assert any(e["type"] == "deadline_miss" for e in tr.events)
    assert any(e["type"] == "step_cancel" and e["code"] == E.EXEC_CANCELLED for e in tr.events)


def test_backend_unavailable(registry, accept_plan):
    res, ev = go(accept_plan, registry, faults=["backend_down:actuator.pulse"])
    assert res.code == E.EXEC_BACKEND_UNAVAILABLE


def test_bad_fault_spec():
    import pytest

    with pytest.raises(ValueError):
        FaultInjection.parse(["nonsense"])


def test_golden_trace_matches(registry, accept_plan):
    _, ev = go(accept_plan, registry)
    golden = ROOT / "traces" / "golden" / "parallel_sense_accept.ndjson"
    expected = [json.loads(l) for l in golden.read_text().splitlines() if l.strip()]
    got = [{"seq": 0, "t_ms": 0, **admit(accept_plan, registry).to_event()}] + [
        {**e, "seq": e["seq"] + 1} for e in ev
    ]
    assert got == expected, "golden trace drifted; regenerate with `make golden` if intentional"
