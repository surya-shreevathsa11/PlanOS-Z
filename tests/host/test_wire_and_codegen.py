"""Wire format v1, `compile --emit uart|c`, forging, codegen and the Python worker-bound model."""

import json
import shutil
import subprocess

import pytest

from planoszc import codegen, wire
from planoszc.admission import Policy, admit, load_plan, wcet_bound
from planoszc.cli import main
from planoszc.compile import CompileError, compile_plan, forge_ir
from planoszc.executor import run as run_plan
from planoszc.idl import load_registry
from planoszc.trace import Trace

from conftest import MISSIONS, ROOT


def ir_of(registry, mission="04_parallel_overlap_accept.yaml", workers=None):
    return compile_plan(admit(load_plan(MISSIONS / mission), registry, Policy(workers=workers)), registry)


# ------------------------------------------------------------------------------- wire
def test_crc16_ccitt_false_check_value():
    assert wire.crc16_ccitt(b"123456789") == 0x29B1


def test_wire_roundtrip(registry):
    ir = ir_of(registry)
    blob = wire.encode(ir)
    back = wire.decode(blob)
    assert back["plan_id"] == ir["plan_id"] and back["deadline_ms"] == ir["deadline_ms"]
    assert back["perm_mask"] == ir["perm_mask"]
    for a, b in zip(ir["steps"], back["steps"]):
        for k in ("id", "skill", "deps_mask", "confirm", "when_step", "when_op", "when_value_milli", "args", "wcet_ms"):
            assert int(a[k]) == int(b[k]) if not isinstance(a[k], (str, list)) else a[k] == b[k]
        assert b["excl_mask"] == b["shared_mask"] == b["mutex_mask"] == 0  # masks are NOT on the wire


@pytest.mark.parametrize("mission", ["01_parallel_sense_accept.yaml", "02_deadline_reject.yaml",
                                     "03_resource_conflict_reject.yaml", "04_parallel_overlap_accept.yaml"])
def test_every_mission_fits_the_upload_buffer(registry, mission):
    assert len(wire.encode(ir_of(registry, mission))) <= wire.UPLOAD_MAX_BYTES


def test_wire_rejects_corruption(registry):
    blob = wire.encode(ir_of(registry))
    for i in range(0, len(blob), 7):
        bad = bytearray(blob)
        bad[i] ^= 0x10
        with pytest.raises(wire.WireError):
            wire.decode(bytes(bad))
    with pytest.raises(wire.WireError):
        wire.decode(wire.encode(ir_of(registry), version=2))
    with pytest.raises(wire.WireError):
        wire.decode(blob[:-1])


def test_wire_limits_are_enforced_at_encode(registry):
    ir = ir_of(registry)
    ir["plan_id"] = "x" * 40
    with pytest.raises(wire.WireError):
        wire.encode(ir)


def test_shell_lines_shape(registry):
    lines = wire.shell_lines(wire.encode(ir_of(registry)))
    assert lines[0] == "planosz load begin" and lines[-1] == "planosz load end"
    assert all(l.startswith("planosz load data ") and len(l.split()[-1]) <= 192 for l in lines[1:-1])


def test_cli_compile_uart_and_forge(capsys, tmp_path):
    mission = str(MISSIONS / "02_deadline_reject.yaml")
    assert main(["compile", mission, "--emit", "uart", "--force"]) == 0
    out = capsys.readouterr().out.splitlines()
    assert out[0] == "planosz load begin" and out[-1] == "planosz load end"
    assert main(["compile", mission, "--emit", "json", "--force", "--forge", "understate-wcet,strip-masks"]) == 0
    ir = json.loads(capsys.readouterr().out)
    assert all(s["wcet_ms"] == 1 and s["excl_mask"] == 0 for s in ir["steps"])
    assert main(["compile", mission, "--emit", "json", "--force", "--forge", "nonsense"]) != 0
    # refuses to emit a plan the host itself rejects unless forced
    assert main(["compile", mission, "--emit", "uart"]) != 0


def test_forge_ir_rejects_unknown_modes(registry):
    with pytest.raises(CompileError):
        forge_ir(ir_of(registry), ["bogus"])


# ------------------------------------------------------------------- compile restrictions
def test_when_on_non_primary_or_non_numeric_output_is_not_compilable(registry):
    plan = {"plan_id": "t.when", "deadline_ms": 3000, "permissions": ["READ_SENSOR", "DIAG"],
            "steps": [{"id": "a", "action": "sensor.read", "args": {"sensor": "cell_temp"}},
                      {"id": "b", "action": "diag.report", "args": {}, "needs": ["a"],
                       "when": {"ref": "a.unit", "op": "==", "value": "degC"}}]}
    adm = admit(plan, registry)
    assert adm.accepted
    with pytest.raises(CompileError):
        compile_plan(adm, registry)


# ----------------------------------------------------------------------------- codegen
def test_generated_tables_are_fresh(registry):
    hdr = (ROOT / "runtime/include/planosz/skill_table.gen.h").read_text()
    src = (ROOT / "runtime/src/registry/skill_table.gen.c").read_text()
    assert hdr == codegen.gen_header(registry) and src == codegen.gen_source(registry)


@pytest.mark.parametrize("wcet", ["samples / 2", "1.5 * samples", "samples * 0.5 + 1", "40 + 20 * value"])
def test_codegen_refuses_what_it_cannot_prove_integer_exact(registry, wcet):
    import dataclasses

    from planoszc import expr as ex

    base = registry.get("sensor.read")
    bad = dataclasses.replace(base, wcet_src=wcet, _wcet=ex.parse(wcet))
    if wcet.startswith("40"):
        bad = dataclasses.replace(bad, args={**base.args, "value": {"type": "float"}})
    with pytest.raises(codegen.CodegenError):
        codegen.wcet_to_c(bad)


def test_codegen_emits_a_wcet_function_per_skill(registry):
    src = codegen.gen_source(registry)
    for c in registry.all():
        assert f".wcet_fn = wcet_{c.skill_id.replace('.', '_')}" in src


def test_resource_universe_is_sorted_and_fits_a_word(registry):
    u = codegen.resource_universe(registry)
    assert u == sorted(u) and len(u) <= 32 and "out_warn_lamp" in u and "adc0" in u


# ------------------------------------------------------------------------- worker bounds
def test_wcet_bound_model():
    assert wcet_bound(230, 630, 2, None) == 230  # unbounded: critical path
    assert wcet_bound(230, 630, 2, 1) == 630  # one worker: sum
    assert wcet_bound(230, 630, 2, 2) == 230  # omega <= workers: critical path
    assert wcet_bound(230, 630, 3, 2) == 230 + (630 - 230 + 1) // 2  # Graham, ceil
    assert wcet_bound(230, 630, 3, 2) <= 630


@pytest.mark.parametrize("workers,expect", [(None, 230), (3, 230), (2, 430), (1, 630)])
def test_parallel_mission_bound_by_workers(registry, workers, expect):
    plan = {"plan_id": "t.par", "deadline_ms": 5000, "permissions": ["READ_SENSOR", "DIAG"],
            "steps": [{"id": f"s{i}", "action": "sensor.read", "args": {"sensor": "cell_temp", "samples": 8}}
                      for i in range(3)]}
    adm = admit(plan, registry, Policy(workers=workers))
    assert adm.max_concurrency == 3 and adm.critical_path_ms == 200 and adm.wcet_ms == {None: 200, 3: 200, 2: 400, 1: 600}[workers]


def test_accept_mission_bounds(registry):
    plan = load_plan(MISSIONS / "01_parallel_sense_accept.yaml")
    assert admit(plan, registry).wcet_ms == 1180
    assert admit(plan, registry, Policy(workers=2)).wcet_ms == 1180
    assert admit(plan, registry, Policy(workers=1)).wcet_ms == 1550
    assert Policy(sequential_executor=True).effective_workers == 1


def test_python_executor_respects_max_workers(registry):
    adm = admit(load_plan(MISSIONS / "04_parallel_overlap_accept.yaml"), registry, Policy(workers=2))
    from planoszc.executor import FakeBackend

    totals = {}
    for w in (None, 2, 1):
        tr = Trace()
        res = run_plan(adm, FakeBackend(), tr, max_workers=w)
        totals[w] = res.total_ms
        starts = [e for e in tr.events if e["type"] == "step_start"]
        assert all("wcet_ms" in e and "claims" in e for e in starts)
    assert totals[1] > totals[2] >= totals[None]


@pytest.mark.skipif(shutil.which("gcc") is None, reason="gcc not available")
def test_cli_compile_c_header_mentions_that_masks_are_ignored(capsys):
    assert main(["compile", str(MISSIONS / "04_parallel_overlap_accept.yaml"), "--emit", "c"]) == 0
    assert "ignores" in capsys.readouterr().out
