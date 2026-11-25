"""Compile and run the C runtime core on the host with gcc (real pthreads for the parallel
executor), and cross-check it against Python."""

import shutil
import subprocess

import pytest

from planoszc import wire
from planoszc.admission import admit, load_plan
from planoszc.cli import main
from planoszc.compile import compile_plan, forge_ir

from conftest import MISSIONS, ROOT

pytestmark = pytest.mark.skipif(shutil.which("gcc") is None, reason="gcc not available")

RUNTIME_SOURCES = [
    "runtime/src/errors.c",
    "runtime/src/registry/registry.c",
    "runtime/src/registry/skill_table.gen.c",
    "runtime/src/admission/admission.c",
    "runtime/src/resource/resource.c",
    "runtime/src/trace/trace.c",
    "runtime/src/scheduler/executor.c",
    "runtime/src/ingress/ingress.c",
    "runtime/src/ingress/ir_wire.c",
    "runtime/src/osal/osal_posix.c",
    "skills/src/skills.c",
]
GCC = ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread", f"-I{ROOT / 'runtime/include'}"]


def chunks_c(name, blob):
    lines = [f"static const char *const {name}[] = {{"]
    lines += [f'\t"{c}",' for c in [l.split(" ", 3)[3] for l in wire.shell_lines(blob)[1:-1]]]
    lines += ["\tNULL", "};"]
    return "\n".join(lines)


def make_blobs_header(registry, path):
    def ir_of(mission, **kw):
        adm = admit(load_plan(MISSIONS / mission), registry)
        return compile_plan(adm, registry)

    good = wire.encode(ir_of("04_parallel_overlap_accept.yaml"))
    deadline_ir = _force_ir(registry, "02_deadline_reject.yaml")
    conflict_ir = _force_ir(registry, "03_resource_conflict_reject.yaml")
    forged = [wire.encode(forge_ir(deadline_ir, ["understate-wcet"])),
              wire.encode(forge_ir(conflict_ir, ["understate-wcet"]))]

    bad = {}
    flipped = bytearray(good)
    flipped[20] ^= 0x01
    bad["crc mismatch (bit flip)"] = bytes(flipped)
    bad["unsupported version (valid CRC)"] = wire.encode(ir_of("04_parallel_overlap_accept.yaml"), version=2)
    bad["unsupported flags (valid CRC)"] = wire.encode(ir_of("04_parallel_overlap_accept.yaml"), flags=1)
    bad["truncated"] = good[:-9]
    bad["bad magic"] = b"XX" + good[2:]
    # trailing garbage with a CRC that covers it
    body = good[:-2] + b"\x00\x00"
    bad["trailing bytes (valid CRC)"] = body + wire.crc16_ccitt(body).to_bytes(2, "little")
    # structurally absurd but CRC-valid headers
    for why, mutate in (("zero steps", lambda b: b[:12] + b"\x00" + b[13:]),
                        ("33 steps", lambda b: b[:12] + b"\x21" + b[13:]),
                        ("unsafe plan id char", lambda b: b[:15] + b'"' + b[16:])):
        raw = bytearray(good[:-2])
        raw = bytearray(mutate(bytes(raw)))
        bad[why] = bytes(raw) + wire.crc16_ccitt(bytes(raw)).to_bytes(2, "little")
    bad["empty"] = b""

    out = ["#include \"planosz/planosz.h\"", "struct blob { const char *plan_id; const char *why; enum planosz_code expect;",
           "\tconst char *const *chunks; };"]
    out.append(chunks_c("GOOD_0", good))
    out.append("static const struct blob GOOD[] = {{ \"demo.parallel_overlap\", \"parallel\", PLANOSZ_OK, GOOD_0 }};")
    out.append("#define N_GOOD 1")
    names = []
    for i, (why, blob) in enumerate(bad.items()):
        out.append(chunks_c(f"BAD_{i}", blob) if blob else f"static const char *const BAD_{i}[] = {{ NULL }};")
        names.append(f'{{ "-", "{why}", PLANOSZ_REJECT_UNSUPPORTED_IR, BAD_{i} }}')
    out.append("static const struct blob BAD[] = {" + ", ".join(names) + "};")
    out.append(f"#define N_BAD {len(names)}")
    out.append(chunks_c("FORGED_0", forged[0]))
    out.append(chunks_c("FORGED_1", forged[1]))
    out.append('static const struct blob FORGED[] = {{ "demo.deadline_reject", "deadline", '
               'PLANOSZ_REJECT_DEADLINE_INFEASIBLE, FORGED_0 }, { "demo.resource_conflict", "conflict", '
               "PLANOSZ_REJECT_RESOURCE_CONFLICT, FORGED_1 }};")
    out.append("#define N_FORGED 2")
    path.write_text("\n".join(out) + "\n")


def _force_ir(registry, mission):
    adm = admit(load_plan(MISSIONS / mission), registry)
    return compile_plan(adm, registry)


@pytest.fixture(scope="session")
def c_runtime_exe(tmp_path_factory, registry):
    tmp = tmp_path_factory.mktemp("c_runtime")
    for sym, name, mission in (
        ("ACCEPT", "accept", "01_parallel_sense_accept"),
        ("DEADLINE", "deadline", "02_deadline_reject"),
        ("CONFLICT", "conflict", "03_resource_conflict_reject"),
        ("PARALLEL", "parallel", "04_parallel_overlap_accept"),
    ):
        rc = main(["compile", str(MISSIONS / f"{mission}.yaml"), "--emit", "c", "--force", "--symbol", sym,
                   "-o", str(tmp / f"{name}.h")])
        assert rc == 0
    make_blobs_header(registry, tmp / "blobs.h")
    exe = tmp / "test_runtime"
    subprocess.run([*GCC, f"-I{tmp}", "tests/host/c/test_runtime.c", *RUNTIME_SOURCES, "-o", str(exe)],
                   check=True, cwd=ROOT)
    return exe


@pytest.fixture(scope="session")
def oracle_exe(tmp_path_factory):
    exe = tmp_path_factory.mktemp("oracle") / "oracle"
    subprocess.run([*GCC, "tests/host/c/oracle.c", *[s for s in RUNTIME_SOURCES if "executor" not in s
                                                      and "ingress.c" not in s and "osal" not in s
                                                      and "trace" not in s and "skills.c" not in s],
                    "-o", str(exe)], check=True, cwd=ROOT)
    return exe


@pytest.fixture(scope="session")
def c_runtime_run(c_runtime_exe):
    return subprocess.run([str(c_runtime_exe)], capture_output=True, text=True, timeout=120)


def test_c_core_matches_python_decisions(c_runtime_run, registry):
    out = c_runtime_run
    assert out.returncode == 0, out.stdout
    assert "RESULT PASS" in out.stdout
    decisions = {}
    for line in out.stdout.splitlines():
        if line.startswith("ADMIT "):
            _, name, code, *rest = line.split()
            decisions[name] = (code, rest)
    from planoszc.admission import Policy

    for name, mission in (("accept", "01_parallel_sense_accept"), ("deadline", "02_deadline_reject"),
                          ("conflict", "03_resource_conflict_reject")):
        py = admit(load_plan(MISSIONS / f"{mission}.yaml"), registry, Policy(workers=2))
        assert decisions[name][0] == py.code, f"C and Python disagree on {mission}"
        if py.wcet_ms is not None and decisions[name][1]:
            assert decisions[name][1][0] == f"wcet={py.wcet_ms}"
    py = admit(load_plan(MISSIONS / "04_parallel_overlap_accept.yaml"), registry, Policy(workers=2))
    assert decisions["parallel_w2"] == (py.code, [f"wcet={py.wcet_ms}"]) and py.wcet_ms == 430


def test_c_tests_report_real_overlap(c_runtime_run):
    out = c_runtime_run.stdout
    line = next(l for l in out.splitlines() if l.startswith("TIMING"))
    par, seq = (int(x.rstrip("ms")) for x in (p.split("=")[1] for p in line.split()[1:]))
    assert par < 0.85 * seq, line


def test_c_upload_rejects_are_all_exercised(c_runtime_run):
    out = c_runtime_run.stdout
    bad = [l for l in out.splitlines() if l.startswith("BADLOAD")]
    assert len(bad) >= 10 and all(l.endswith("REJECT_UNSUPPORTED_IR") for l in bad)
