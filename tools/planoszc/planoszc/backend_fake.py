"""Host fake backend: deterministic simulated skills (no wall-clock, no hardware)."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

from .idl import Contract

DEFAULT_SENSORS = {
    "pack_voltage": (48.2, "V"),
    "coolant_proxy": (31.2, "degC"),
    "cell_temp": (27.5, "degC"),
    "ambient_temp": (24.0, "degC"),
}


class SkillFailure(Exception):
    def __init__(self, code: str, detail: str):
        super().__init__(detail)
        self.code = code
        self.detail = detail


@dataclass
class FaultInjection:
    sensor_fail: set[str] = field(default_factory=set)
    slow_ms: dict[str, float] = field(default_factory=dict)  # skill_id -> forced duration
    backend_down: set[str] = field(default_factory=set)  # skill_ids

    @classmethod
    def parse(cls, specs: list[str]) -> "FaultInjection":
        fi = cls()
        for s in specs:
            kind, _, rest = s.partition(":")
            if kind == "sensor_fail" and rest:
                fi.sensor_fail.add(rest)
            elif kind == "slow" and "=" in rest:
                skill, _, ms = rest.partition("=")
                fi.slow_ms[skill] = float(ms)
            elif kind == "backend_down" and rest:
                fi.backend_down.add(rest)
            else:
                raise ValueError(
                    f"bad --inject '{s}' (use sensor_fail:<sensor> | slow:<skill>=<ms> | backend_down:<skill>)"
                )
        return fi


class FakeBackend:
    def __init__(self, sensors: dict[str, float] | None = None, faults: FaultInjection | None = None):
        self.sensor_values = {k: v[0] for k, v in DEFAULT_SENSORS.items()}
        self.sensor_values.update(sensors or {})
        self.faults = faults or FaultInjection()
        self.records_emitted = 0

    def duration_ms(self, contract: Contract, args: dict[str, Any]) -> float:
        if contract.skill_id in self.faults.slow_ms:
            return self.faults.slow_ms[contract.skill_id]
        return contract.typical_ms(args)

    def check_available(self, contract: Contract) -> None:
        if contract.skill_id in self.faults.backend_down:
            raise SkillFailure("EXEC_BACKEND_UNAVAILABLE", f"backend for {contract.skill_id} is down (injected)")

    def run_skill(self, contract: Contract, args: dict[str, Any], now_ms: float) -> dict[str, Any]:
        sid = contract.skill_id
        if sid == "sensor.read":
            name = args["sensor"]
            if name in self.faults.sensor_fail:
                raise SkillFailure("EXEC_SKILL_FAIL", f"sensor '{name}' read failed (injected)")
            return {"value": self.sensor_values[name], "unit": DEFAULT_SENSORS[name][1]}
        if sid == "actuator.pulse":
            return {"pulses": args["count"]}
        if sid == "wait":
            return {"waited_ms": args["duration_ms"]}
        if sid == "power.idle":
            return {"slept_ms": args["duration_ms"]}
        if sid == "sys.info":
            text = {
                "uptime": f"{int(now_ms)}ms",
                "version": "planosz-sim-0.1.0",
                "board": "host_fake",
                "heap": "65536",
            }[args["field"]]
            return {"text": text}
        if sid == "diag.report":
            self.records_emitted += 1
            return {"records": 1 if args["scope"] == "summary" else 4}
        if sid == "actuator.contactor":
            return {"state": args["state"]}
        raise SkillFailure("EXEC_BACKEND_UNAVAILABLE", f"no host implementation for {sid}")
