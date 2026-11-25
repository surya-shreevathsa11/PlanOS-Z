"""Differential tests: the C device runtime (built from the real sources, driven through
tests/host/c/oracle.c) must agree with the Python reference on WCET, resource masks, admission
decisions and the wire format. This is what lets the host admission be a faithful *preview* of
the device's decision."""

import itertools
import random
import subprocess

import pytest

from planoszc import wire
from planoszc.admission import Policy, admit
from planoszc.codegen import device_masks, resource_universe
from planoszc.compile import CompileError, compile_plan, encode_arg, forge_ir

from conftest import MISSIONS
from test_c_runtime import oracle_exe  # noqa: F401  (session fixture)
from planoszc.admission import load_plan


def oracle(exe, script: str) -> list[str]:
    out = subprocess.run([str(exe)], input=script, capture_output=True, text=True, timeout=60, check=True)
    return out.stdout.splitlines()


# --------------------------------------------------------------------------- per-skill derivation
def _arg_values(spec: dict) -> list:
    t = spec["type"]
    if t == "enum":
        return list(spec["values"])
    if t == "bool":
        return [False, True]
    from planoszc.idl import INT_BOUNDS

    lo, hi = INT_BOUNDS.get(t, (-(2**31), 2**31 - 1))
    lo, hi = max(lo, spec.get("min", lo)), min(hi, spec.get("max", hi))
    vals = {lo, hi, (lo + hi) // 2, min(lo + 1, hi), max(hi - 1, lo)}
    if t == "float":
        vals = {float(v) for v in vals}
    return sorted(vals)


def test_device_derivation_matches_python_for_every_skill(oracle_exe, registry):  # noqa: F811
    lines, expected = [], []
    for c in registry.all():
        names = list(c.args)
        for combo in itertools.product(*[_arg_values(c.args[n]) for n in names]):
            resolved = dict(zip(names, combo))
            wcet, excl, shared = device_masks(registry, c, resolved)
            enc = [encode_arg(c.args[n], resolved[n]) for n in names] + [0] * (4 - len(names))
            lines.append(f"derive {c.index} {' '.join(map(str, enc))}")
            expected.append((c.skill_id, resolved, wcet, excl, shared))
    got = [l for l in oracle(oracle_exe, "\n".join(lines) + "\n") if l.startswith("D ")]
    assert len(got) == len(expected) and len(expected) > 100
    for line, (sid, resolved, wcet, excl, shared) in zip(got, expected):
        assert line == f"D {wcet} {excl} {shared}", f"{sid} {resolved}"


def test_device_rejects_out_of_range_args(oracle_exe, registry):  # noqa: F811
    wait = registry.get("wait")
    script = []
    for v in (0, 1, 60000, 60001, -5):
        script.append("plan 100000 0 0 4294967295 0")
        script.append(f"step {wait.index} 0 0 -1 0 0 {v} 0 0 0 1 0 0 0")
        script.append("admit")
    script.append("plan 100000 0 0 4294967295 0")
    script.append("step 99 0 0 -1 0 0 0 0 0 0 1 0 0 0")
    script.append("admit")
    got = [l.split()[1] for l in oracle(oracle_exe, "\n".join(script) + "\n") if l.startswith("A ")]
    assert got == ["REJECT_BAD_ARGS", "OK", "OK", "REJECT_BAD_ARGS", "REJECT_BAD_ARGS", "REJECT_UNKNOWN_SKILL"]


def test_device_resource_space_equals_codegen_universe(oracle_exe, registry):  # noqa: F811
    got = [l.split()[2] for l in oracle(oracle_exe, "resources\n") if l.startswith("R ")]
    assert got == resource_universe(registry)


# --------------------------------------------------------------------------- admission fuzz
SAFE_STEPS = [
    ("sensor.read", lambda r: {"sensor": r.choice(["pack_voltage", "coolant_proxy", "cell_temp", "ambient_temp"]),
                               "samples": r.randint(1, 16)}),
    ("actuator.pulse", lambda r: {"channel": r.choice(["warn_lamp", "cooling_fan", "buzzer"]),
                                  "count": r.randint(1, 6), "on_ms": r.choice([10, 200, 900]),
                                  "off_ms": r.choice([10, 200, 900])}),
    ("diag.report", lambda r: {"scope": r.choice(["summary", "full"])}),
    ("power.idle", lambda r: {"duration_ms": r.choice([1, 50, 400, 3000]), "mode": r.choice(["light", "deep"])}),
    ("sys.info", lambda r: {"field": r.choice(["uptime", "version", "board", "heap"])}),
    ("wait", lambda r: {"duration_ms": r.choice([1, 100, 700, 5000])}),
    ("actuator.contactor", lambda r: {"state": r.choice(["close", "open"]), "settle_ms": r.choice([10, 300])}),
]
ALL_PERMS = ["READ_SENSOR", "ACTUATE", "POWER_CONTROL", "DIAG", "HV_CONTROL"]


def random_plan(rng: random.Random, idx: int) -> dict:
    n = rng.randint(1, 9)
    steps = []
    for i in range(n):
        action, mk = rng.choice(SAFE_STEPS)
        st = {"id": f"s{i}", "action": action, "args": mk(rng)}
        if i and rng.random() < 0.55:
            st["needs"] = sorted(rng.sample([f"s{j}" for j in range(i)], rng.randint(1, min(3, i))), key=lambda x: int(x[1:]))
        producers = [s for s in steps if s["action"] == "sensor.read"]
        if producers and rng.random() < 0.35:
            ref = rng.choice(producers)["id"]
            st["when"] = {"ref": f"{ref}.value", "op": rng.choice([">", "<=", "<", ">=", "==", "!="]),
                          "value": rng.choice([0, 12.5, 30])}
            st.setdefault("needs", [])
            if ref not in st["needs"]:
                st["needs"] = st["needs"] + [ref]
        if action == "actuator.contactor" and rng.random() < 0.8:
            st["confirm"] = True
        steps.append(st)
    perms = list(ALL_PERMS) if rng.random() < 0.85 else rng.sample(ALL_PERMS, rng.randint(1, 4))
    return {"plan_id": f"fuzz.{idx}", "deadline_ms": rng.choice([300, 800, 1500, 4000, 20000]),
            "permissions": perms, "steps": steps}


def _oracle_plan(ir: dict, deadline: int, workers: int, allow_haz: bool) -> str:
    out = [f"plan {deadline} {ir['perm_mask']} {workers} 4294967295 {int(allow_haz)}"]
    for s in ir["steps"]:
        a = (s["args"] + [0] * 4)[:4]
        out.append(f"step {s['skill']} {s['deps_mask']} {int(s['confirm'])} {s['when_step']} {s['when_op']} "
                   f"{s['when_value_milli']} {a[0]} {a[1]} {a[2]} {a[3]} {s['wcet_ms']} {s['excl_mask']} "
                   f"{s['shared_mask']} {s['mutex_mask']}")
    out.append("admit")
    return "\n".join(out)


def test_random_dags_c_and_python_admission_agree(oracle_exe, registry):  # noqa: F811
    rng = random.Random(0xA11CE)
    script, expect, seen = [], [], {}
    for idx in range(900):
        plan = random_plan(rng, idx)
        for workers in (0, 1, 2, 3):
            adm = admit(plan, registry, Policy(workers=workers or None))
            if not adm.steps or adm.code in ("REJECT_INVALID_PLAN", "REJECT_BAD_ARGS", "REJECT_CYCLE",
                                             "REJECT_UNKNOWN_SKILL", "REJECT_DEP_MISSING"):
                continue  # structurally invalid: never reaches IR
            try:
                ir = compile_plan(adm, registry)
            except CompileError:
                continue
            script.append(_oracle_plan(ir, plan["deadline_ms"], workers, False))
            expect.append((plan["plan_id"], workers, adm))
            seen[adm.code] = seen.get(adm.code, 0) + 1
    got = [l for l in oracle(oracle_exe, "\n".join(script) + "\n") if l.startswith("A ")]
    assert len(got) == len(expect) and len(expect) > 1500
    for line, (pid, workers, adm) in zip(got, expect):
        _, code, _step, _other, wcet, cp, omega, mismatch = line.split()
        assert code == adm.code, f"{pid} workers={workers}: C says {code}, Python says {adm.code}"
        assert int(mismatch) == 0, "an honestly compiled IR never mismatches the device WCET"
        if adm.code in ("OK", "REJECT_DEADLINE_INFEASIBLE"):
            assert int(wcet) == adm.wcet_ms, f"{pid} workers={workers}"
            assert int(cp) == adm.critical_path_ms and int(omega) == adm.max_concurrency, pid
    # the fuzz actually reaches the interesting decisions
    for needed in ("OK", "REJECT_DEADLINE_INFEASIBLE", "REJECT_RESOURCE_CONFLICT"):
        assert seen.get(needed, 0) > 50, seen


@pytest.mark.parametrize("modes", [["understate-wcet"], ["strip-masks"], ["understate-wcet", "strip-masks"]])
def test_forged_ir_gets_the_same_decision_as_the_honest_ir(oracle_exe, registry, modes):  # noqa: F811
    """The device ignores host wcet/masks: forging them can never turn a REJECT into an ACCEPT."""
    rng = random.Random(7)
    honest_script, forged_script = [], []
    for idx in range(250):
        plan = random_plan(rng, idx)
        adm = admit(plan, registry, Policy(workers=2))
        if not adm.steps or adm.code not in ("OK", "REJECT_DEADLINE_INFEASIBLE", "REJECT_RESOURCE_CONFLICT"):
            continue
        ir = compile_plan(adm, registry)
        honest_script.append(_oracle_plan(ir, plan["deadline_ms"], 2, False))
        forged_script.append(_oracle_plan(forge_ir(ir, modes), plan["deadline_ms"], 2, False))
    honest = [l.split() for l in oracle(oracle_exe, "\n".join(honest_script) + "\n") if l.startswith("A ")]
    forged = [l.split() for l in oracle(oracle_exe, "\n".join(forged_script) + "\n") if l.startswith("A ")]
    assert len(honest) == len(forged) > 100
    for h, f in zip(honest, forged):
        assert h[:7] == f[:7], (h, f)  # code, step, other, wcet, cp, omega identical
    if "understate-wcet" in modes:
        assert any(int(f[7]) > 0 for f in forged)  # ... and the lie is reported


def test_c_rejects_a_missing_or_unsafe_permission(oracle_exe, registry):  # noqa: F811
    plan = {"plan_id": "t.haz", "deadline_ms": 5000, "permissions": ["ACTUATE", "HV_CONTROL"],
            "steps": [{"id": "c", "action": "actuator.contactor", "args": {"state": "close"}, "confirm": True}]}
    adm = admit(plan, registry, Policy(allow_hazardous=True))
    assert adm.accepted
    ir = compile_plan(adm, registry)
    assert oracle(oracle_exe, _oracle_plan(ir, 5000, 0, False)).pop().split()[1] == "REJECT_SAFETY_CLASS"
    assert oracle(oracle_exe, _oracle_plan(ir, 5000, 0, True)).pop().split()[1] == "OK"
    ir["perm_mask"] = 0  # tampered: claims no permissions yet holds a skill that needs them
    assert oracle(oracle_exe, _oracle_plan(ir, 5000, 0, True)).pop().split()[1] == "REJECT_PERMISSION"


# --------------------------------------------------------------------------- wire format
def test_wire_roundtrip_python_and_c_agree(oracle_exe, registry):  # noqa: F811
    for mission in sorted(MISSIONS.glob("0[1-4]_*.yaml")):
        adm = admit(load_plan(mission), registry)
        if not adm.steps:
            continue
        ir = compile_plan(adm, registry)
        blob = wire.encode(ir)
        assert wire.decode(blob)["steps"][0]["id"] == ir["steps"][0]["id"]
        chunks = [l.split(" ", 3)[3] for l in wire.shell_lines(blob)[1:-1]]
        out = oracle(oracle_exe, "wire " + " ".join(chunks) + " wire-end\n")
        assert out[0] == "W OK ", out[0]
        assert out[1] == f"P {ir['plan_id']} {ir['deadline_ms']} {ir['perm_mask']} {len(ir['steps'])}"
        for line, s in zip(out[2:], ir["steps"]):
            f = line.split()
            assert f[1] == s["id"] and int(f[2]) == s["skill"] and int(f[3]) == s["deps_mask"]
            assert [int(x) for x in f[8:12]][: len(s["args"])] == s["args"]
            assert int(f[6]) == s["when_op"] and int(f[7]) == s["when_value_milli"] and int(f[12]) == s["wcet_ms"]


def test_wire_c_rejects_what_python_rejects(oracle_exe, registry):  # noqa: F811
    adm = admit(load_plan(MISSIONS / "04_parallel_overlap_accept.yaml"), registry)
    good = wire.encode(compile_plan(adm, registry))
    cases = {
        "bitflip": bytes(b ^ 1 if i == 20 else b for i, b in enumerate(good)),
        "version": wire.encode(compile_plan(adm, registry), version=9),
        "short": good[:10],
    }
    for name, blob in cases.items():
        with pytest.raises(wire.WireError):
            wire.decode(blob)
        chunks = [l.split(" ", 3)[3] for l in wire.shell_lines(blob)[1:-1]]
        out = oracle(oracle_exe, "wire " + " ".join(chunks) + " wire-end\n")
        assert out[0].startswith("W REJECT_UNSUPPORTED_IR"), (name, out[0])
