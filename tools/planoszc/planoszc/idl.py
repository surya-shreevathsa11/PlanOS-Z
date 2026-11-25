"""Skill IDL loader: YAML contracts -> validated Contract objects + registry."""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import yaml
from jsonschema import Draft202012Validator

from . import expr

INT_BOUNDS = {
    "uint8": (0, 255),
    "uint16": (0, 65535),
    "uint32": (0, 4294967295),
    "int32": (-2147483648, 2147483647),
}


class IdlError(Exception):
    pass


def find_repo_root(start: Path | None = None) -> Path | None:
    """Locate the repo root (the directory containing idl/skills)."""
    candidates = [Path(start)] if start else []
    candidates += [Path.cwd(), Path(__file__).resolve()]
    for base in candidates:
        for parent in [base, *base.parents]:
            if (parent / "idl" / "skills").is_dir():
                return parent
    return None


def default_idl_dir() -> Path:
    env = os.environ.get("PLANOSZ_IDL_DIR")
    if env:
        return Path(env)
    root = find_repo_root()
    if root is None:
        raise IdlError(
            "cannot find idl/skills; run inside the PlanOS-Z repo or pass --idl / set PLANOSZ_IDL_DIR"
        )
    return root / "idl" / "skills"


def _schema(name: str, idl_dir: Path) -> dict:
    path = idl_dir.parent / "schemas" / name
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def load_plan_schema(idl_dir: Path) -> dict:
    return _schema("plan.schema.json", idl_dir)


@dataclass(frozen=True)
class ResourceClaim:
    template: str
    mode: str  # exclusive | shared

    def resolve(self, args: dict[str, Any]) -> str:
        try:
            return self.template.format(**args)
        except KeyError as exc:
            raise IdlError(f"resource template {self.template!r} references unknown arg {exc}") from exc


@dataclass
class Contract:
    skill_id: str
    version: int
    summary: str
    args: dict[str, dict]
    returns: dict[str, dict]
    resources: list[ResourceClaim]
    wcet_src: str
    typical_src: str
    power_class: str
    safety_class: str
    permissions: list[str]
    index: int = 0
    _wcet: Any = field(default=None, repr=False)
    _typical: Any = field(default=None, repr=False)
    source: str = ""

    # ---- argument handling -------------------------------------------------
    def resolve_args(self, given: dict[str, Any] | None) -> tuple[dict[str, Any], list[str]]:
        """Apply defaults and validate. Returns (resolved, errors)."""
        given = dict(given or {})
        errors: list[str] = []
        out: dict[str, Any] = {}
        for name in given:
            if name not in self.args:
                errors.append(f"unknown arg '{name}'")
        for name, spec in self.args.items():
            if name in given:
                value = given[name]
            elif "default" in spec:
                value = spec["default"]
            else:
                errors.append(f"missing required arg '{name}'")
                continue
            err = _check_value(name, spec, value)
            if err:
                errors.append(err)
            else:
                out[name] = value
        return out, errors

    def wcet_ms(self, resolved: dict[str, Any]) -> float:
        return float(expr.evaluate(self._wcet, resolved))

    def typical_ms(self, resolved: dict[str, Any]) -> float:
        return float(expr.evaluate(self._typical, resolved))

    def claims(self, resolved: dict[str, Any]) -> list[tuple[str, str]]:
        return [(c.resolve(resolved), c.mode) for c in self.resources]


def _check_value(name: str, spec: dict, value: Any) -> str | None:
    t = spec["type"]
    if t in INT_BOUNDS:
        if isinstance(value, bool) or not isinstance(value, int):
            return f"arg '{name}' must be {t}, got {value!r}"
        lo, hi = INT_BOUNDS[t]
        lo = max(lo, spec.get("min", lo))
        hi = min(hi, spec.get("max", hi))
        if not lo <= value <= hi:
            return f"arg '{name}'={value} out of range [{lo}, {hi}]"
    elif t == "float":
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            return f"arg '{name}' must be float, got {value!r}"
        if "min" in spec and value < spec["min"]:
            return f"arg '{name}'={value} below min {spec['min']}"
        if "max" in spec and value > spec["max"]:
            return f"arg '{name}'={value} above max {spec['max']}"
    elif t == "bool":
        if not isinstance(value, bool):
            return f"arg '{name}' must be bool, got {value!r}"
    elif t == "enum":
        if value not in spec["values"]:
            return f"arg '{name}'={value!r} not in {spec['values']}"
    elif t == "string":
        if not isinstance(value, str):
            return f"arg '{name}' must be string, got {value!r}"
    return None


class Registry:
    def __init__(self, contracts: list[Contract], idl_dir: Path):
        self.idl_dir = idl_dir
        self._by_id = {c.skill_id: c for c in contracts}
        for i, c in enumerate(sorted(contracts, key=lambda c: c.skill_id)):
            c.index = i

    def __contains__(self, skill_id: str) -> bool:
        return skill_id in self._by_id

    def get(self, skill_id: str) -> Contract | None:
        return self._by_id.get(skill_id)

    def all(self) -> list[Contract]:
        return sorted(self._by_id.values(), key=lambda c: c.index)

    def permissions(self) -> list[str]:
        """Sorted universe of permission names (bit index = position in this list)."""
        return sorted({p for c in self._by_id.values() for p in c.permissions})


def load_registry(idl_dir: Path | str | None = None) -> Registry:
    idl_dir = Path(idl_dir) if idl_dir else default_idl_dir()
    if not idl_dir.is_dir():
        raise IdlError(f"IDL directory not found: {idl_dir}")
    validator = Draft202012Validator(_schema("contract.schema.json", idl_dir))
    contracts: list[Contract] = []
    seen: set[str] = set()
    for path in sorted(idl_dir.glob("*.yaml")):
        with open(path, encoding="utf-8") as fh:
            doc = yaml.safe_load(fh)
        errs = sorted(validator.iter_errors(doc), key=lambda e: list(e.path))
        if errs:
            raise IdlError(f"{path.name}: " + "; ".join(e.message for e in errs))
        if doc["skill_id"] in seen:
            raise IdlError(f"{path.name}: duplicate skill_id {doc['skill_id']}")
        seen.add(doc["skill_id"])
        for aname, spec in doc["args"].items():
            if spec["type"] == "enum" and "values" not in spec:
                raise IdlError(f"{path.name}: enum arg '{aname}' needs values")
            if "default" in spec:
                e = _check_value(aname, spec, spec["default"])
                if e:
                    raise IdlError(f"{path.name}: bad default: {e}")
        try:
            wtree = expr.parse(doc["wcet_ms"])
            ttree = expr.parse(doc.get("typical_ms", doc["wcet_ms"]))
        except expr.ExprError as exc:
            raise IdlError(f"{path.name}: {exc}") from exc
        for tree, label in ((wtree, "wcet_ms"), (ttree, "typical_ms")):
            unknown = expr.names_in(tree) - set(doc["args"])
            if unknown:
                raise IdlError(f"{path.name}: {label} references unknown args {sorted(unknown)}")
        contracts.append(
            Contract(
                skill_id=doc["skill_id"],
                version=doc["version"],
                summary=doc["summary"],
                args=doc["args"],
                returns=doc["returns"],
                resources=[ResourceClaim(r["name"], r["mode"]) for r in doc["resources"]],
                wcet_src=str(doc["wcet_ms"]),
                typical_src=str(doc.get("typical_ms", doc["wcet_ms"])),
                power_class=doc["power_class"],
                safety_class=doc["safety_class"],
                permissions=list(doc["permissions"]),
                _wcet=wtree,
                _typical=ttree,
                source=path.name,
            )
        )
    if not contracts:
        raise IdlError(f"no *.yaml contracts in {idl_dir}")
    return Registry(contracts, idl_dir)
