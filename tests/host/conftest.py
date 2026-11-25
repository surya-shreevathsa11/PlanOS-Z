import copy
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "planoszc"))

from planoszc.admission import load_plan  # noqa: E402
from planoszc.idl import load_registry  # noqa: E402

MISSIONS = ROOT / "examples" / "missions"


@pytest.fixture(scope="session")
def registry():
    return load_registry(ROOT / "idl" / "skills")


@pytest.fixture()
def accept_plan():
    return copy.deepcopy(load_plan(MISSIONS / "01_parallel_sense_accept.yaml"))


@pytest.fixture()
def base_plan():
    """A tiny valid plan to mutate in negative tests."""
    return {
        "plan_id": "t.base",
        "deadline_ms": 1000,
        "permissions": ["READ_SENSOR", "ACTUATE"],
        "steps": [
            {"id": "a", "action": "sensor.read", "args": {"sensor": "pack_voltage"}},
            {"id": "b", "action": "actuator.pulse", "args": {"channel": "buzzer", "count": 2}, "needs": ["a"]},
        ],
    }
