"""CLI behaviour: exit codes, output contract, NDJSON validity."""

import json
import subprocess
import sys

from conftest import MISSIONS, ROOT

from planoszc.cli import main


def cli(*args):
    return main([*map(str, args)])


def test_admit_accept(capsys):
    assert cli("admit", MISSIONS / "01_parallel_sense_accept.yaml") == 0
    assert "ACCEPTED  demo.parallel_sense" in capsys.readouterr().out


def test_admit_rejects_with_reason_code_and_exit_2(capsys):
    assert cli("admit", MISSIONS / "02_deadline_reject.yaml") == 2
    out = capsys.readouterr().out
    assert "REJECTED  REJECT_DEADLINE_INFEASIBLE" in out
    assert "estimated_wcet_ms=2594 deadline_ms=2000" in out
    assert cli("admit", MISSIONS / "03_resource_conflict_reject.yaml") == 2
    assert "REJECTED  REJECT_RESOURCE_CONFLICT" in capsys.readouterr().out


def test_admit_json_is_one_ndjson_line(capsys):
    cli("admit", MISSIONS / "02_deadline_reject.yaml", "--format", "json")
    lines = capsys.readouterr().out.strip().splitlines()
    assert len(lines) == 1
    ev = json.loads(lines[0])
    assert ev["code"] == "REJECT_DEADLINE_INFEASIBLE" and ev["type"] == "admit"


def test_check_prints_step_table(capsys):
    assert cli("check", MISSIONS / "01_parallel_sense_accept.yaml") == 0
    out = capsys.readouterr().out
    assert "done_by" in out and "out_warn_lamp:ex" in out


def test_run_human(capsys):
    assert cli("run", MISSIONS / "01_parallel_sense_accept.yaml") == 0
    out = capsys.readouterr().out
    assert "TRACE" in out and "RESULT: ok" in out and "skip   rest" in out


def test_run_rejected_does_not_execute(capsys):
    assert cli("run", MISSIONS / "03_resource_conflict_reject.yaml") == 2
    out = capsys.readouterr().out
    assert "REJECTED" in out and "step_start" not in out and "start " not in out


def test_run_ndjson_all_lines_valid_json(capsys, tmp_path):
    f = tmp_path / "t.ndjson"
    assert cli("run", MISSIONS / "01_parallel_sense_accept.yaml", "--format", "ndjson", "--trace", f) == 0
    lines = capsys.readouterr().out.strip().splitlines()
    evs = [json.loads(l) for l in lines]
    assert evs[0]["type"] == "admit" and evs[-1]["type"] == "run_end"
    assert f.read_text().strip().splitlines() == lines


def test_run_fault_exit_code(capsys):
    assert cli("run", MISSIONS / "01_parallel_sense_accept.yaml", "--inject", "sensor_fail:coolant_proxy") == 3
    assert "EXEC_SKILL_FAIL" in capsys.readouterr().out


def test_run_sensor_override(capsys):
    cli("run", MISSIONS / "01_parallel_sense_accept.yaml", "--sensor", "coolant_proxy=20")
    assert "skip   warn" in capsys.readouterr().out


def test_missing_file_is_usage_error(capsys):
    assert cli("admit", "/nonexistent/plan.yaml") == 1
    assert "cannot read plan" in capsys.readouterr().err


def test_policy_flags(capsys, tmp_path):
    plan = tmp_path / "p.yaml"
    plan.write_text(
        "plan_id: t.haz\ndeadline_ms: 1000\npermissions: [ACTUATE, HV_CONTROL]\nsteps:\n"
        "  - {id: c, action: actuator.contactor, args: {state: close}}\n"
    )
    assert cli("admit", plan) == 2
    assert "REJECT_SAFETY_CLASS" in capsys.readouterr().out
    assert cli("admit", plan, "--allow-hazardous") == 0
    assert cli("admit", plan, "--allow-hazardous", "--grant", "ACTUATE") == 2
    assert "REJECT_PERMISSION" in capsys.readouterr().out


def test_skills_listing(capsys):
    assert cli("skills") == 0
    out = capsys.readouterr().out
    for s in ("sensor.read", "actuator.pulse", "wait", "power.idle", "sys.info", "diag.report"):
        assert s in out


def test_module_entrypoint():
    r = subprocess.run([sys.executable, "-m", "planoszc", "--version"], capture_output=True, text=True,
                       cwd=ROOT)
    assert r.returncode == 0 and "planoszc" in r.stdout


def test_executor_flag_changes_timing_model(capsys, tmp_path):
    import yaml

    plan = yaml.safe_load((MISSIONS / "01_parallel_sense_accept.yaml").read_text())
    plan["deadline_ms"] = 1300
    f = tmp_path / "tight.yaml"
    f.write_text(yaml.safe_dump(plan))
    assert cli("admit", f) == 0
    assert cli("admit", f, "--executor", "sequential") == 2
    assert "REJECT_DEADLINE_INFEASIBLE" in capsys.readouterr().out
