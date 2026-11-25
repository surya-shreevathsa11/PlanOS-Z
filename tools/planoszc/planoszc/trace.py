"""NDJSON trace events (one JSON object per line, stable key order)."""

from __future__ import annotations

import json
from typing import Any, Callable, Iterable


def _num(x: Any) -> Any:
    if isinstance(x, float):
        return int(x) if x == int(x) else round(x, 3)
    return x


class Trace:
    def __init__(self, sink: Callable[[str], None] | None = None):
        self.events: list[dict] = []
        self._sink = sink

    def emit(self, ev: dict) -> dict:
        head = {k: ev[k] for k in ("type", "t_ms") if k in ev}
        rest = {k: v for k, v in ev.items() if k not in head}
        ev = {"seq": len(self.events), **{k: _num(v) for k, v in {**head, **rest}.items()}}
        self.events.append(ev)
        if self._sink:
            self._sink(dumps(ev))
        return ev

    def lines(self) -> list[str]:
        return [dumps(e) for e in self.events]

    def text(self) -> str:
        return "\n".join(self.lines()) + "\n"


def dumps(ev: dict) -> str:
    return json.dumps(ev, separators=(",", ":"), ensure_ascii=False)


def loads(lines: Iterable[str]) -> list[dict]:
    return [json.loads(l) for l in lines if l.strip()]
