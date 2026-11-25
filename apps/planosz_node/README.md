# planosz_node

The Zephyr application image for PlanOS-Z. It links the runtime module from the repo root,
registers the demo plans in `src/plan_*.gen.h` (compiled from `examples/missions` by `planoszc`;
the `*_forged` ones lie about WCET / resource masks on purpose), runs them on a 2-worker parallel
executor, and exposes `planosz status|skills|plans|admit|run|load` on the shell
(`load` = IR upload over the UART, see `docs/uart_upload.md`).

    # from the repo root, after `make zephyr-setup`
    make native-sim-demo   # build + run the 6 demo plans (ACCEPT, parallel overlap, 4 REJECTs)
    make native-sim-shell  # shell on stdin/stdout for the upload test
    make esp32-build       # build for esp32_devkitc/esp32/procpu (needs the xtensa toolchain)

Regenerate the compiled plans after editing a mission: `make app-plans`.
