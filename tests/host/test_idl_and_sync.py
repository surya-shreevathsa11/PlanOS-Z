"""IDL contracts, codegen freshness, and doc/code sync."""

import json
import re

import pytest

from planoszc import errors as E
from planoszc import expr
from planoszc.codegen import gen_header, gen_source
from planoszc.idl import IdlError, load_registry

from conftest import ROOT


def test_at_least_six_skills(registry):
    ids = {c.skill_id for c in registry.all()}
    assert {"sensor.read", "actuator.pulse", "wait", "power.idle", "sys.info", "diag.report"} <= ids


def test_every_contract_has_schema_fields(registry):
    for c in registry.all():
        assert c.version >= 1 and c.summary and c.safety_class and c.power_class
        assert c.wcet_ms({k: (v.get("min", 1) if v["type"] != "enum" else v["values"][0])
                          for k, v in c.args.items()}) > 0


def test_idl_schemas_are_valid_json_schema():
    from jsonschema import Draft202012Validator

    for n in ("contract.schema.json", "plan.schema.json"):
        Draft202012Validator.check_schema(json.loads((ROOT / "idl" / "schemas" / n).read_text()))


def test_bad_contract_rejected(tmp_path):
    (tmp_path / "skills").mkdir()
    (tmp_path / "schemas").mkdir()
    for n in ("contract.schema.json", "plan.schema.json"):
        (tmp_path / "schemas" / n).write_text((ROOT / "idl" / "schemas" / n).read_text())
    (tmp_path / "skills" / "x.yaml").write_text(
        "skill_id: x.y\nversion: 1\nsummary: s\nargs: {a: {type: uint8}}\nreturns: {}\nresources: []\n"
        "wcet_ms: 'a + zzz'\npower_class: SLEEP\nsafety_class: SAFE\npermissions: []\n"
    )
    with pytest.raises(IdlError, match="unknown args"):
        load_registry(tmp_path / "skills")


def test_expr_is_not_python_eval():
    for bad in ("__import__('os')", "a.b", "[1]", "(lambda: 1)()", "a ** 2"):
        with pytest.raises(expr.ExprError):
            expr.parse(bad)
    assert expr.evaluate(expr.parse("min(a, 3) + (1 if b == 'x' else 2)"), {"a": 5, "b": "x"}) == 4


def test_generated_c_registry_is_fresh(registry):
    hdr = (ROOT / "runtime/include/planosz/skill_table.gen.h").read_text()
    src = (ROOT / "runtime/src/registry/skill_table.gen.c").read_text()
    assert hdr == gen_header(registry) and src == gen_source(registry), "run `planoszc codegen`"


def test_error_codes_in_sync_across_python_c_docs():
    doc = (ROOT / "docs/error_codes.md").read_text()
    hdr = (ROOT / "runtime/include/planosz/errors.h").read_text()
    src = (ROOT / "runtime/src/errors.c").read_text()
    enum_vals = dict(re.findall(r"PLANOSZ_(\w+) = (\d+),", hdr))
    for i, code in enumerate(E.ALL_CODES):
        assert code in doc, f"{code} undocumented"
        assert f'"{code}"' in src, f"{code} missing in errors.c"
        assert enum_vals[code] == str(i), f"{code} numeric value drifted"
    assert len(E.ALL_CODES) == len(enum_vals)
