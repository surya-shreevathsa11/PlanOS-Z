"""`planoszc replay`: the golden traces pass, and every kind of tampered / impossible trace is caught
by the rule that is meant to catch it."""

import copy
import json

import pytest

from planoszc.admission import load_plan
from planoszc.cli import main
from planoszc.replay import ReplayError, check_run, parse_lines, replay, split_runs

from conftest import MISSIONS, ROOT

GOLDEN = ROOT / "traces" / "golden"
OVERLAP = GOLDEN / "parallel_overlap_w2.ndjson"
SENSE = GOLDEN / "parallel_sense_accept.ndjson"


def load(path):
    return parse_lines(path.read_text().splitlines())


@pytest.fixture()
def overlap_run():
    return copy.deepcopy(load(OVERLAP))


@pytest.fixture()
def overlap_plan():
    return load_plan(MISSIONS / "04_parallel_overlap_accept.yaml")


def rules(events, plan=None, registry=None, **kw):
    return sorted({v.rule for v in check_run(events, plan, registry, **kw).violations})


def ev(events, **match):
    return next(e for e in events if all(e.get(k) == v for k, v in match.items()))


def test_golden_traces_pass_with_and_without_the_plan(registry, overlap_plan):
    assert rules(load(OVERLAP)) == []
    assert rules(load(OVERLAP), overlap_plan, registry, require_overlap=2) == []
    assert rules(load(SENSE), load_plan(MISSIONS / "01_parallel_sense_accept.yaml"), registry) == []
    rep = check_run(load(OVERLAP))
    assert rep.peak_concurrency == 2 and rep.steps_ok == 4 and rep.status == "OK"


def test_cli_replay_exit_codes(capsys, tmp_path):
    assert main(["replay", str(OVERLAP), "--plan", str(MISSIONS / "04_parallel_overlap_accept.yaml"),
                 "--require-overlap", "demo.parallel_overlap=2"]) == 0
    assert "0 violation(s)" in capsys.readouterr().out
    assert main(["replay", str(OVERLAP), "--require-overlap", "demo.parallel_overlap=3"]) == 4
    bad = tmp_path / "bad.ndjson"
    bad.write_text("this is not json\n")
    assert main(["replay", str(bad)]) == 4
    assert main(["replay", str(tmp_path / "missing.ndjson")]) == 1
    empty = tmp_path / "empty.ndjson"
    empty.write_text("")
    assert main(["replay", str(empty)]) == 4


def test_dependency_violation_is_caught(overlap_run):
    report = ev(overlap_run, type="step_start", step="report")
    report["t_ms"] = 60  # starts while `cell` is still running (it ends at 100)
    assert "causality" in rules(overlap_run)


def test_step_started_before_its_dependency_even_started(overlap_run):
    overlap_run[:] = [e for e in overlap_run if not (e.get("step") == "cell")]
    for i, e in enumerate(overlap_run):
        e["seq"] = i
    assert "causality" in rules(overlap_run)


def test_wcet_overrun_is_caught(overlap_run):
    end = ev(overlap_run, type="step_end", step="volt")
    end["t_ms"] = 400
    end["dur_ms"] = 400
    got = rules(overlap_run)
    assert "wcet" in got


def test_dur_ms_that_disagrees_with_timestamps_is_caught(overlap_run):
    ev(overlap_run, type="step_end", step="volt")["dur_ms"] = 7
    assert "wcet" in rules(overlap_run)


def test_exclusive_resource_overlap_is_caught(overlap_run):
    for e in overlap_run:
        if e.get("step") in ("volt", "cool") and e["type"] == "step_start":
            e["claims"] = ["adc0:exclusive", "power_domain:shared"]
    assert "resources" in rules(overlap_run)


def test_non_monotonic_time_and_seq_gaps_are_caught(overlap_run):
    ev(overlap_run, type="step_end", step="cell")["t_ms"] = 3
    assert "seq" in rules(overlap_run) or "wcet" in rules(overlap_run)
    gap = copy.deepcopy(load(OVERLAP))
    gap[4]["seq"] = 9
    assert "seq" in rules(gap)


def test_missing_run_end_and_double_end(overlap_run):
    assert "lifecycle" in rules(overlap_run[:-1])
    dup = copy.deepcopy(overlap_run)
    dup.insert(5, {**ev(dup, type="step_end", step="volt")})
    for i, e in enumerate(dup):
        e["seq"] = i
    assert "lifecycle" in rules(dup)


def test_run_that_claims_ok_but_left_a_step_unaccounted(overlap_run):
    kept = [e for e in overlap_run if e.get("step") != "cell"]
    for i, e in enumerate(kept):
        e["seq"] = i
    assert "lifecycle" in rules(kept) or "causality" in rules(kept)


def test_work_after_a_reject_is_caught():
    events = [
        {"seq": 0, "type": "admit", "t_ms": 0, "plan": "x", "status": "REJECT", "code": "REJECT_DEADLINE_INFEASIBLE",
         "wcet_ms": 9, "deadline_ms": 1},
        {"seq": 1, "type": "run_start", "t_ms": 0, "plan": "x", "deadline_ms": 1, "steps": 1},
    ]
    assert "admission" in rules(events)
    assert rules(events[:1]) == []  # a bare REJECT is the correct trace for a rejected plan


def test_deadline_overrun_is_caught(overlap_run):
    ev(overlap_run, type="run_end")["t_ms"] = 900
    ev(overlap_run, type="run_end")["total_ms"] = 900
    assert "deadline" in rules(overlap_run)


def test_more_workers_in_flight_than_admitted(overlap_run):
    ev(overlap_run, type="admit")["workers"] = 1
    assert "concurrency" in rules(overlap_run)


def test_require_overlap_fails_for_a_sequential_trace():
    seq = load(OVERLAP)
    # serialise: volt, then cool, then cell
    ev(seq, type="step_start", step="cool")["t_ms"] = 50
    ev(seq, type="step_end", step="cool")["t_ms"] = 100
    ev(seq, type="step_start", step="cell")["t_ms"] = 100
    ev(seq, type="step_end", step="cell")["t_ms"] = 150
    ev(seq, type="step_end", step="cool")["dur_ms"] = 50
    ev(seq, type="step_start", step="report")["t_ms"] = 150
    ev(seq, type="step_end", step="report")["t_ms"] = 160
    ev(seq, type="run_end")["t_ms"] = 160
    ev(seq, type="run_end")["total_ms"] = 160
    seq.sort(key=lambda e: (e["t_ms"], e["seq"]))
    for i, e in enumerate(seq):
        e["seq"] = i
    assert rules(seq) == []  # a legal sequential execution ...
    assert "concurrency" in rules(seq, require_overlap=2)  # ... but not the overlap claimed


def test_plan_aware_checks(overlap_run, overlap_plan, registry):
    admit = ev(overlap_run, type="admit")
    admit["wcet_ms"] = 10  # lies about the bound
    assert "plan" in rules(overlap_run, overlap_plan, registry)
    tampered = copy.deepcopy(load(OVERLAP))
    ev(tampered, type="step_start", step="volt")["wcet_ms"] = 5
    assert "plan" in rules(tampered, overlap_plan, registry) or "wcet" in rules(tampered, overlap_plan, registry)


def test_condition_checks_catch_a_wrong_branch(registry):
    events = load(SENSE)
    plan = load_plan(MISSIONS / "01_parallel_sense_accept.yaml")
    assert rules(events, plan, registry) == []
    # the coolant reading was 31.2 > 30, so `warn` ran and `rest` was skipped. Swap them.
    swapped = copy.deepcopy(events)
    for e in swapped:
        if e.get("step") == "warn":
            e["step"] = "rest"
        elif e.get("step") == "rest":
            e["step"] = "warn"
    assert "condition" in rules(swapped, plan, registry) or "plan" in rules(swapped, plan, registry)


def test_extract_mode_pulls_events_out_of_a_console_log():
    log = ["*** Booting Zephyr OS ***", "[sim] hello", 'uart:~$ {"seq":0,"type":"admit","t_ms":0,"plan":"p"}',
           "garbage {not json", json.dumps({"seq": 1, "type": "run_end"})]
    evs = parse_lines(log, extract=True)
    assert [e["type"] for e in evs] == ["admit", "run_end"] and len(split_runs(evs)) == 1
    with pytest.raises(ReplayError):
        parse_lines(log)


def test_load_records_stand_alone_and_runs_are_split():
    events = [
        {"seq": 0, "type": "load", "t_ms": 0, "plan": "uploaded.x", "status": "OK", "code": "OK", "bytes": 120},
        {"seq": 0, "type": "load", "t_ms": 0, "plan": "-", "status": "REJECT", "code": "REJECT_UNSUPPORTED_IR",
         "detail": "crc mismatch", "bytes": 120},
    ]
    reports = replay(events)
    assert [r.kind for r in reports] == ["load", "load"] and not any(r.violations for r in reports)
    events[1]["code"] = "OK"  # a REJECT that claims OK is inconsistent
    assert any(r.violations for r in replay(events))


def test_tolerance_ms_allows_hardware_jitter(overlap_run):
    ev(overlap_run, type="step_end", step="volt")["dur_ms"] = 205
    ev(overlap_run, type="step_end", step="volt")["t_ms"] = 205
    assert "wcet" in rules(overlap_run)
    assert "wcet" not in rules(overlap_run, tolerance_ms=10)
