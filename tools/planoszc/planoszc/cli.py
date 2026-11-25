"""planoszc command line interface."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from . import __version__
from . import errors as E
from .admission import Admission, Policy, PlanLoadError, admit, load_plan
from .backend_fake import FakeBackend, FaultInjection
from .codegen import gen_header, gen_source
from .compile import FORGE_MODES, CompileError, compile_plan, forge_ir, ir_to_c_header
from .executor import run as run_plan
from .idl import IdlError, Registry, find_repo_root, load_registry
from .replay import ReplayError, parse_lines, replay
from .trace import Trace, dumps
from .wire import WireError, encode as wire_encode, shell_lines


# ------------------------------------------------------------------ formatting
def _fmt_args(args: dict) -> str:
    return ", ".join(f"{k}={v}" for k, v in args.items())


def format_admission(adm: Admission) -> str:
    if adm.accepted:
        return (
            f"ACCEPTED  {adm.plan_id}\n"
            f"  steps={len(adm.steps)} estimated_wcet_ms={adm.wcet_ms} deadline_ms={adm.deadline_ms}\n"
            f"  critical_path={'->'.join(adm.critical_path)}"
        )
    lines = [f"REJECTED  {adm.code}", f"  plan:   {adm.plan_id}", f"  detail: {adm.primary_detail()}"]
    for v in adm.violations[1:]:
        lines.append(f"  also:   {v.code}: {v.detail}")
    return "\n".join(lines)


def format_analysis(adm: Admission) -> str:
    out = [format_admission(adm)]
    if adm.steps and adm.accepted:
        out.append("")
        out.append(f"  {'step':<10}{'skill':<20}{'wcet_ms':>8}{'done_by':>9}  resources")
        for sid in adm.order:
            s = adm.step(sid)
            res = ", ".join(f"{n}:{m[:2]}" for n, m in s.claims) or "-"
            cond = f"  [when {s.when['ref']} {s.when['op']} {s.when['value']}]" if s.when else ""
            out.append(f"  {s.id:<10}{s.action:<20}{s.wcet_ms:>8}{s.earliest_finish_ms:>9}  {res}{cond}")
        out.append("  (done_by = worst-case finish along dependencies; conditional branches counted)")
    return "\n".join(out)


def format_event(ev: dict) -> str:
    t = ev.get("t_ms", 0)
    ts = f"[{t:>6} ms]"
    k = ev["type"]
    if k == "admit":
        return f"{ts} admit {ev['status'].lower()} ({ev['code']})"
    if k == "run_start":
        return f"{ts} run_start plan={ev['plan']} deadline_ms={ev['deadline_ms']}"
    if k == "step_start":
        return f"{ts} start  {ev['step']:<8} {ev['action']}({_fmt_args(ev['args'])})"
    if k == "step_end":
        return f"{ts} end    {ev['step']:<8} ok dur={ev['dur_ms']}ms out={json.dumps(ev['out'], separators=(',', ':'))}"
    if k == "step_skip":
        return f"{ts} skip   {ev['step']:<8} cond false: {ev['cond']} (observed {ev['observed']})"
    if k == "step_error":
        return f"{ts} ERROR  {ev['step']:<8} {ev['code']}: {ev['detail']}"
    if k == "step_cancel":
        return f"{ts} cancel {ev['step']:<8} {ev['code']}"
    if k == "deadline_miss":
        return f"{ts} DEADLINE MISS deadline_ms={ev['deadline_ms']}"
    if k == "run_end":
        return f"{ts} run_end {ev['status']} code={ev['code']} total={ev['total_ms']}ms"
    return f"{ts} {k}"


# ------------------------------------------------------------------ helpers
def _registry(args: argparse.Namespace) -> Registry:
    return load_registry(args.idl)


def _policy(args: argparse.Namespace) -> Policy:
    grant = getattr(args, "grant", None)
    return Policy(
        granted_permissions=set(grant.split(",")) if grant else None,
        allow_hazardous=getattr(args, "allow_hazardous", False),
        sequential_executor=getattr(args, "executor", "parallel") == "sequential",
        workers=getattr(args, "workers", None),
    )


def _load_and_admit(args: argparse.Namespace, policy: Policy | None = None):
    reg = _registry(args)
    plan = load_plan(args.plan)
    return reg, plan, admit(plan, reg, policy)


# ------------------------------------------------------------------ commands
def cmd_check(args: argparse.Namespace) -> int:
    _, _, adm = _load_and_admit(args, Policy(sequential_executor=args.executor == "sequential",
                                             workers=args.workers))
    if args.format == "json":
        ev = adm.to_event()
        if adm.accepted:
            ev["critical_path"] = adm.critical_path
            ev["step_detail"] = [
                {"id": s.id, "action": s.action, "wcet_ms": s.wcet_ms, "done_by_ms": s.earliest_finish_ms,
                 "resources": [{"name": n, "mode": m} for n, m in s.claims]}
                for s in (adm.step(i) for i in adm.order)
            ]
        print(dumps(ev))
    else:
        print(format_analysis(adm))
    return E.EXIT_OK if adm.accepted else E.EXIT_REJECTED


def cmd_admit(args: argparse.Namespace) -> int:
    _, _, adm = _load_and_admit(args, _policy(args))
    if args.format == "json":
        print(dumps({"seq": 0, "t_ms": 0, **adm.to_event()}))
    else:
        print(format_admission(adm))
    return E.EXIT_OK if adm.accepted else E.EXIT_REJECTED


def cmd_run(args: argparse.Namespace) -> int:
    reg, _, adm = _load_and_admit(args, _policy(args))
    ndjson = args.format == "ndjson"
    try:
        faults = FaultInjection.parse(args.inject or [])
        sensors: dict[str, float] = {}
        for item in args.sensor or []:
            name, _, val = item.partition("=")
            sensors[name] = float(val)
    except ValueError as exc:
        print(f"planoszc: {exc}", file=sys.stderr)
        return E.EXIT_USAGE
    trace_file = open(args.trace, "w", encoding="utf-8") if args.trace else None

    def sink(line: str) -> None:
        if trace_file:
            trace_file.write(line + "\n")
        if ndjson:
            print(line)

    trace = Trace(sink)
    trace.emit({"t_ms": 0, **adm.to_event()})
    try:
        if not adm.accepted:
            if not ndjson:
                print(format_admission(adm))
            return E.EXIT_REJECTED
        if not ndjson:
            print(format_admission(adm))
            print("TRACE")
        before = len(trace.events)
        result = run_plan(adm, FakeBackend(sensors, faults), trace,
                          max_workers=_policy(args).effective_workers)
        if not ndjson:
            for ev in trace.events[before:]:
                print("  " + format_event(ev))
            if result.status == "OK":
                print(f"RESULT: ok  total_ms={result.total_ms:g} deadline_ms={adm.deadline_ms}")
            else:
                print(f"RESULT: FAILED  {result.code}  at t={result.total_ms:g}ms")
        return E.EXIT_OK if result.status == "OK" else E.EXIT_EXEC_FAILED
    finally:
        if trace_file:
            trace_file.close()


def cmd_compile(args: argparse.Namespace) -> int:
    reg, _, adm = _load_and_admit(args, _policy(args))
    if not adm.accepted and not args.force:
        print(format_admission(adm), file=sys.stderr)
        print("planoszc: refusing to compile a rejected plan (use --force to test device-side admission)",
              file=sys.stderr)
        return E.EXIT_REJECTED
    try:
        ir = compile_plan(adm, reg)
        if args.forge:
            ir = forge_ir(ir, args.forge.split(","))
        if args.emit == "uart":
            text = "\n".join(shell_lines(wire_encode(ir))) + "\n"
        elif args.emit == "c":
            text = ir_to_c_header(ir, args.symbol)
        else:
            text = json.dumps(ir, indent=2) + "\n"
    except (CompileError, WireError) as exc:
        print(f"planoszc: {exc}", file=sys.stderr)
        return E.EXIT_USAGE
    if args.output:
        Path(args.output).write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    return E.EXIT_OK


def cmd_replay(args: argparse.Namespace) -> int:
    reg = None
    plans: dict[str, dict] = {}
    if args.plan:
        reg = _registry(args)
        for path in args.plan:
            doc = load_plan(path)
            plans[str(doc.get("plan_id"))] = doc
    overlap: dict[str, int] = {}
    for item in args.require_overlap or []:
        name, _, n = item.partition("=")
        if not n.isdigit():
            print(f"planoszc: --require-overlap wants PLAN_ID=N, got '{item}'", file=sys.stderr)
            return E.EXIT_USAGE
        overlap[name] = int(n)
    try:
        src = sys.stdin if args.trace == "-" else open(args.trace, encoding="utf-8")
        with src:
            events = parse_lines(src, extract=args.extract)
    except OSError as exc:
        print(f"planoszc: cannot read trace {args.trace}: {exc}", file=sys.stderr)
        return E.EXIT_USAGE
    except ReplayError as exc:
        print(f"planoszc: replay: {exc}", file=sys.stderr)
        return E.EXIT_REPLAY_FAILED
    if not events:
        print("planoszc: replay: no trace events found", file=sys.stderr)
        return E.EXIT_REPLAY_FAILED
    reports = replay(events, plans, reg, tolerance_ms=args.tolerance_ms, require_overlap=overlap)
    bad = 0
    for rep in reports:
        print(rep.summary())
        for v in rep.violations:
            print(f"       {v}")
            bad += 1
    runs = sum(1 for r in reports if r.kind == "run")
    print(f"replay: {len(reports)} record(s) ({runs} run(s)), {bad} violation(s)")
    return E.EXIT_OK if bad == 0 else E.EXIT_REPLAY_FAILED


def cmd_skills(args: argparse.Namespace) -> int:
    reg = _registry(args)
    if args.format == "json":
        for c in reg.all():
            print(dumps({"skill_id": c.skill_id, "version": c.version, "args": list(c.args),
                         "resources": [f"{r.template}:{r.mode}" for r in c.resources],
                         "wcet_ms": c.wcet_src, "power_class": c.power_class,
                         "safety_class": c.safety_class, "permissions": c.permissions}))
        return E.EXIT_OK
    print(f"{'skill':<20}{'v':<3}{'safety':<18}{'power':<12}{'permissions':<22}resources")
    for c in reg.all():
        res = ", ".join(f"{r.template}:{r.mode[:2]}" for r in c.resources) or "-"
        print(f"{c.skill_id:<20}{c.version:<3}{c.safety_class:<18}{c.power_class:<12}"
              f"{','.join(c.permissions) or '-':<22}{res}")
        print(f"  wcet_ms = {c.wcet_src}")
    return E.EXIT_OK


def cmd_codegen(args: argparse.Namespace) -> int:
    reg = _registry(args)
    if args.out_dir:
        root = Path(args.out_dir)
    else:
        found = find_repo_root()
        root = (found or Path.cwd()) / "runtime"
    hdr = root / "include" / "planosz" / "skill_table.gen.h"
    src = root / "src" / "registry" / "skill_table.gen.c"
    hdr.parent.mkdir(parents=True, exist_ok=True)
    src.parent.mkdir(parents=True, exist_ok=True)
    hdr.write_text(gen_header(reg), encoding="utf-8")
    src.write_text(gen_source(reg), encoding="utf-8")
    print(f"wrote {hdr}\nwrote {src}")
    return E.EXIT_OK


# ------------------------------------------------------------------ parser
def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="planoszc",
        description="PlanOS-Z host tool: check, admit and run job packs against skill contracts.",
    )
    p.add_argument("--version", action="version", version=f"planoszc {__version__}")
    p.add_argument("--idl", help="skill IDL directory (default: <repo>/idl/skills or $PLANOSZ_IDL_DIR)")
    sub = p.add_subparsers(dest="cmd", required=True)

    def add_policy(sp: argparse.ArgumentParser) -> None:
        sp.add_argument("--grant", help="comma list of permissions the device grants (default: all)")
        sp.add_argument("--allow-hazardous", action="store_true", help="device policy: allow HAZARDOUS skills")
        sp.add_argument("--executor", choices=["parallel", "sequential"], default="parallel",
                        help="timing model: parallel = critical path (unbounded workers); "
                             "sequential = sum of WCETs (= --workers 1)")
        sp.add_argument("--workers", type=int, metavar="N",
                        help="bound the executor to N workers (the device default is 2): critical path "
                             "if the plan never needs more than N at once, else the list-scheduling bound")

    sp = sub.add_parser("check", help="static analysis: contracts, deps, resources, WCET (no policy)")
    sp.add_argument("plan")
    sp.add_argument("--executor", choices=["parallel", "sequential"], default="parallel")
    sp.add_argument("--workers", type=int, metavar="N")
    sp.add_argument("--format", choices=["human", "json"], default="human")
    sp.set_defaults(fn=cmd_check)

    sp = sub.add_parser("admit", help="admission gate with device policy; prints ACCEPT/REJECT + reason code")
    sp.add_argument("plan")
    sp.add_argument("--format", choices=["human", "json"], default="human")
    add_policy(sp)
    sp.set_defaults(fn=cmd_admit)

    sp = sub.add_parser("run", help="admit, then execute on the deterministic fake host backend")
    sp.add_argument("plan")
    sp.add_argument("--format", choices=["human", "ndjson"], default="human")
    sp.add_argument("--trace", metavar="FILE", help="also write the NDJSON trace to FILE")
    sp.add_argument("--sensor", action="append", metavar="NAME=VALUE", help="override a fake sensor value")
    sp.add_argument("--inject", action="append", metavar="FAULT",
                    help="fault injection: sensor_fail:<sensor> | slow:<skill>=<ms> | backend_down:<skill>")
    add_policy(sp)
    sp.set_defaults(fn=cmd_run)

    sp = sub.add_parser("compile", help="emit IR for an admitted plan: json, a C header, or the "
                                        "`planosz load ...` shell payload for UART upload")
    sp.add_argument("plan")
    sp.add_argument("--emit", "--format", dest="emit", choices=["json", "c", "uart"], default="json",
                    help="uart = paste payload for the device shell (begin / data* / end)")
    sp.add_argument("--forge", metavar="MODES",
                    help=f"test tool: falsify host claims in the IR ({','.join(FORGE_MODES)}) to prove "
                         f"the device re-derives WCET/resources")
    sp.add_argument("-o", "--output")
    sp.add_argument("--symbol", default="PLAN", help="C symbol prefix for --emit c")
    sp.add_argument("--force", action="store_true",
                    help="emit IR even if host admission rejects (resource/deadline only); "
                         "used to prove the device re-rejects it")
    add_policy(sp)
    sp.set_defaults(fn=cmd_compile)

    sp = sub.add_parser("replay", help="validate an NDJSON trace (host run or device console log): "
                                       "causality, resources, WCET, deadline, concurrency")
    sp.add_argument("trace", help="NDJSON file, or - for stdin")
    sp.add_argument("--plan", action="append", metavar="YAML",
                    help="plan the trace claims to run (repeatable); enables contract/condition checks")
    sp.add_argument("--extract", action="store_true",
                    help="input is a raw device console log: ignore lines that are not trace events")
    sp.add_argument("--require-overlap", action="append", metavar="PLAN_ID=N",
                    help="fail unless a run of PLAN_ID had >= N steps in flight at once (repeatable)")
    sp.add_argument("--tolerance-ms", type=float, default=0.0,
                    help="slack for timing checks on real hardware (default 0)")
    sp.set_defaults(fn=cmd_replay)

    sp = sub.add_parser("skills", help="list registered skill contracts")
    sp.add_argument("--format", choices=["human", "json"], default="human")
    sp.set_defaults(fn=cmd_skills)

    sp = sub.add_parser("codegen", help="generate C registry table from the IDL")
    sp.add_argument("--out-dir", help="runtime directory to write into (default: <repo>/runtime)")
    sp.set_defaults(fn=cmd_codegen)
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.fn(args)
    except (IdlError, PlanLoadError) as exc:
        print(f"planoszc: error: {exc}", file=sys.stderr)
        return E.EXIT_USAGE
    except BrokenPipeError:  # pragma: no cover
        return E.EXIT_OK


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
