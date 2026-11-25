"""Admission control: every reject path has a test asserting the exact reason code."""

import copy

from planoszc import errors as E
from planoszc.admission import Policy, admit, load_plan

from conftest import MISSIONS


def codes(adm):
    return [v.code for v in adm.violations]


def test_base_plan_accepted(registry, base_plan):
    adm = admit(base_plan, registry)
    assert adm.accepted and adm.code == E.OK
    assert adm.wcet_ms == 60 + (2 * 402 + 20)


def test_happy_mission_accepted(registry):
    adm = admit(load_plan(MISSIONS / "01_parallel_sense_accept.yaml"), registry)
    assert adm.accepted
    assert adm.wcet_ms == 1180
    assert adm.critical_path == ["cool", "warn", "report"]


def test_deadline_infeasible_mission(registry):
    adm = admit(load_plan(MISSIONS / "02_deadline_reject.yaml"), registry)
    assert adm.code == E.REJECT_DEADLINE_INFEASIBLE
    assert adm.wcet_ms == 2594
    assert "estimated_wcet_ms=2594 deadline_ms=2000" in adm.primary_detail()


def test_deadline_boundary_is_inclusive(registry, base_plan):
    wcet = admit(base_plan, registry).wcet_ms
    base_plan["deadline_ms"] = wcet
    assert admit(base_plan, registry).accepted
    base_plan["deadline_ms"] = wcet - 1
    assert admit(base_plan, registry).code == E.REJECT_DEADLINE_INFEASIBLE


def test_resource_conflict_mission(registry):
    adm = admit(load_plan(MISSIONS / "03_resource_conflict_reject.yaml"), registry)
    assert adm.code == E.REJECT_RESOURCE_CONFLICT
    assert "out_warn_lamp" in adm.primary_detail()


def test_conflict_fixed_by_ordering(registry):
    plan = load_plan(MISSIONS / "03_resource_conflict_reject.yaml")
    plan["steps"][2]["needs"].append("lamp_a")
    assert admit(plan, registry).accepted


def test_different_channels_do_not_conflict(registry):
    plan = load_plan(MISSIONS / "03_resource_conflict_reject.yaml")
    plan["steps"][2]["args"]["channel"] = "buzzer"
    assert admit(plan, registry).accepted


def test_shared_claims_may_overlap(registry, base_plan):
    base_plan["steps"] = [
        {"id": "a", "action": "sensor.read", "args": {"sensor": "pack_voltage"}},
        {"id": "b", "action": "sensor.read", "args": {"sensor": "cell_temp"}},
    ]
    assert admit(base_plan, registry).accepted  # both share adc0


def test_exclusive_vs_shared_conflicts(registry, base_plan):
    base_plan["permissions"] = ["READ_SENSOR", "POWER_CONTROL"]
    base_plan["steps"] = [
        {"id": "a", "action": "sensor.read", "args": {"sensor": "pack_voltage"}},
        {"id": "b", "action": "power.idle", "args": {"duration_ms": 10}},
    ]
    assert admit(base_plan, registry).code == E.REJECT_RESOURCE_CONFLICT  # power_domain


def test_mutually_exclusive_branches_do_not_conflict(registry, accept_plan):
    # warn (shared power_domain) and rest (exclusive power_domain) are complementary branches
    adm = admit(accept_plan, registry)
    assert adm.accepted and adm.mutex["warn"] == {"rest"}


def test_non_complementary_conditions_do_conflict(registry, accept_plan):
    accept_plan["steps"][3]["when"] = {"ref": "cool.value", "op": "<=", "value": 99}
    assert admit(accept_plan, registry).code == E.REJECT_RESOURCE_CONFLICT


def test_unknown_skill(registry, base_plan):
    base_plan["steps"][0]["action"] = "sensor.teleport"
    adm = admit(base_plan, registry)
    assert adm.code == E.REJECT_UNKNOWN_SKILL and adm.violations[0].step == "a"


def test_bad_args_out_of_range(registry, base_plan):
    base_plan["steps"][1]["args"]["count"] = 51
    assert admit(base_plan, registry).code == E.REJECT_BAD_ARGS


def test_bad_args_missing_unknown_enum_type(registry, base_plan):
    for args in ({}, {"sensor": "warp_core"}, {"sensor": "pack_voltage", "bogus": 1},
                 {"sensor": "pack_voltage", "samples": "3"}, {"sensor": "pack_voltage", "samples": True}):
        base_plan["steps"][0]["args"] = args
        assert admit(base_plan, registry).code == E.REJECT_BAD_ARGS, args


def test_defaults_are_applied(registry, base_plan):
    adm = admit(base_plan, registry)
    assert adm.step("b").args["on_ms"] == 200


def test_permission_not_declared(registry, base_plan):
    base_plan["permissions"] = ["READ_SENSOR"]
    adm = admit(base_plan, registry)
    assert adm.code == E.REJECT_PERMISSION and "ACTUATE" in adm.primary_detail()


def test_permission_not_granted_by_device(registry, base_plan):
    adm = admit(base_plan, registry, Policy(granted_permissions={"READ_SENSOR"}))
    assert adm.code == E.REJECT_PERMISSION


def test_hazardous_skill_rejected_by_default(registry, base_plan):
    base_plan["permissions"] += ["HV_CONTROL"]
    base_plan["deadline_ms"] = 5000
    base_plan["steps"].append({"id": "c", "action": "actuator.contactor", "args": {"state": "close"},
                               "needs": ["b"]})
    adm = admit(base_plan, registry)
    assert adm.code == E.REJECT_SAFETY_CLASS
    assert admit(copy.deepcopy(base_plan), registry, Policy(allow_hazardous=True)).accepted


def test_bad_plan_structure(registry, base_plan):
    p = copy.deepcopy(base_plan)
    p["steps"][1]["needs"] = ["zzz"]
    assert admit(p, registry).code == E.REJECT_BAD_PLAN
    p = copy.deepcopy(base_plan)
    p["steps"][1]["id"] = "a"
    assert admit(p, registry).code == E.REJECT_BAD_PLAN
    p = copy.deepcopy(base_plan)
    p["steps"][0]["needs"] = ["b"]  # a<->b cycle
    adm = admit(p, registry)
    assert adm.code == E.REJECT_BAD_PLAN and "cycle" in adm.primary_detail()
    p = copy.deepcopy(base_plan)
    del p["deadline_ms"]
    assert admit(p, registry).code == E.REJECT_BAD_PLAN
    assert admit({}, registry).code == E.REJECT_BAD_PLAN


def test_when_must_reference_dependency_and_field(registry, accept_plan):
    accept_plan["steps"][2]["when"]["ref"] = "report.records"
    assert admit(accept_plan, registry).code == E.REJECT_BAD_PLAN
    accept_plan["steps"][2]["when"]["ref"] = "cool.nonsense"
    assert admit(accept_plan, registry).code == E.REJECT_BAD_PLAN


def test_all_violations_reported_but_first_decides(registry, base_plan):
    base_plan["steps"][0]["action"] = "nope"
    base_plan["steps"][1]["args"]["count"] = 0
    adm = admit(base_plan, registry)
    assert codes(adm) == [E.REJECT_UNKNOWN_SKILL, E.REJECT_BAD_ARGS]


def test_event_shape(registry):
    ev = admit(load_plan(MISSIONS / "02_deadline_reject.yaml"), registry).to_event()
    assert ev["type"] == "admit" and ev["status"] == "REJECT"
    assert ev["code"] == "REJECT_DEADLINE_INFEASIBLE"
    assert ev["wcet_ms"] == 2594 and ev["deadline_ms"] == 2000


def test_sequential_executor_timing_is_sum_of_wcets(registry, accept_plan):
    par = admit(accept_plan, registry)
    seq = admit(accept_plan, registry, Policy(sequential_executor=True))
    assert par.wcet_ms == 1180 and seq.wcet_ms == 60 + 120 + 1030 + 310 + 30  # 1550
    accept_plan["deadline_ms"] = 1300  # fits the critical path, not the sum
    assert admit(accept_plan, registry).accepted
    adm = admit(accept_plan, registry, Policy(sequential_executor=True))
    assert adm.code == E.REJECT_DEADLINE_INFEASIBLE and "executor=sequential" in adm.primary_detail()
