"""`west planosz ...` - thin wrapper around the host tool, so it works from any west workspace
that uses this repo's west.yml:  west planosz admit planos-z/examples/missions/02_deadline_reject.yaml
"""

import sys
from pathlib import Path

from west.commands import WestCommand


class PlanOSZ(WestCommand):
    def __init__(self):
        super().__init__(
            "planosz",
            "PlanOS-Z job-pack tool",
            "Forwards its arguments to `planoszc` (check, admit, run, compile, skills, codegen).",
            accepts_unknown_args=True,
        )

    def do_add_parser(self, parser_adder):
        parser = parser_adder.add_parser(self.name, help=self.help, description=self.description)
        parser.add_argument("planosz_args", nargs="*", help="arguments for planoszc")
        return parser

    def do_run(self, args, unknown):
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "planoszc"))
        from planoszc.cli import main

        raise SystemExit(main([*args.planosz_args, *unknown]))
