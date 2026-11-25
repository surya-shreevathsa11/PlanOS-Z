# planosz_node

The Zephyr application image for PlanOS-Z. It links the runtime module from the repo root,
registers the demo plans in `src/plan_*.gen.h` (compiled from `examples/missions` by `planoszc`),
and exposes `planosz status|skills|plans|admit|run` on the shell.

    # from the repo root, after `make zephyr-setup`
    make native-sim        # build + run: prints ACCEPT, REJECT, REJECT with NDJSON trace
    make esp32-build       # build for esp32_devkitc/esp32/procpu (needs the xtensa toolchain)

Regenerate the compiled plans after editing a mission: `make app-plans`.
