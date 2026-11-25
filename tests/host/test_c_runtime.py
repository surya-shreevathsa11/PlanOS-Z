"""Compile and run the C runtime core on the host with gcc, and cross-check it against Python."""

import shutil
import subprocess

import pytest

from planoszc.admission import admit, load_plan
from planoszc.cli import main

from conftest import MISSIONS, ROOT

pytestmark = pytest.mark.skipif(shutil.which("gcc") is None, reason="gcc not available")

C_SOURCES = [
    "tests/host/c/test_runtime.c",
    "runtime/src/errors.c",
    "runtime/src/registry/registry.c",
    "runtime/src/registry/skill_table.gen.c",
    "runtime/src/admission/admission.c",
    "runtime/src/resource/resource.c",
    "runtime/src/trace/trace.c",
    "runtime/src/scheduler/executor.c",
    "skills/src/skills.c",
]


def test_c_core_matches_python_decisions(tmp_path, registry):
    for sym, name, mission in (
        ("ACCEPT", "accept", "01_parallel_sense_accept"),
        ("DEADLINE", "deadline", "02_deadline_reject"),
        ("CONFLICT", "conflict", "03_resource_conflict_reject"),
    ):
        rc = main(["compile", str(MISSIONS / f"{mission}.yaml"), "--emit", "c", "--force", "--symbol", sym,
                   "-o", str(tmp_path / f"{name}.h")])
        assert rc == 0
    exe = tmp_path / "test_runtime"
    subprocess.run(
        ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", f"-I{ROOT / 'runtime/include'}", f"-I{tmp_path}",
         *[str(ROOT / s) for s in C_SOURCES], "-o", str(exe)],
        check=True,
    )
    out = subprocess.run([str(exe)], capture_output=True, text=True)
    assert out.returncode == 0, out.stdout
    assert "RESULT PASS" in out.stdout
    decisions = {}
    for line in out.stdout.splitlines():
        if line.startswith("ADMIT "):
            _, name, code, *_ = line.split()
            decisions[name] = code
    for name, mission in (("accept", "01_parallel_sense_accept"), ("deadline", "02_deadline_reject"),
                          ("conflict", "03_resource_conflict_reject")):
        py = admit(load_plan(MISSIONS / f"{mission}.yaml"), registry)
        assert decisions[name] == py.code, f"C and Python disagree on {mission}"
