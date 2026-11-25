# SPDX-License-Identifier: Apache-2.0
#
# Zephyr module entry point (see zephyr/module.yml). Not a standalone CMake project.

if(CONFIG_PLANOSZ)
  zephyr_include_directories(runtime/include)

  zephyr_library_named(planosz)
  zephyr_library_include_directories(runtime/include)
  zephyr_library_sources(
    runtime/src/errors.c
    runtime/src/registry/registry.c
    runtime/src/registry/skill_table.gen.c
    runtime/src/admission/admission.c
    runtime/src/resource/resource.c
    runtime/src/scheduler/executor.c
    runtime/src/trace/trace.c
    runtime/src/ingress/ingress.c
    runtime/src/ingress/ir_wire.c
    runtime/src/osal/osal_zephyr.c
    skills/src/skills.c
  )
  zephyr_library_sources_ifdef(CONFIG_PLANOSZ_SHELL runtime/src/ingress/ingress_shell.c)
  zephyr_library_sources_ifdef(CONFIG_PLANOSZ_BACKEND_NATIVE_SIM backends/native_sim/native_sim_backend.c)
  zephyr_library_sources_ifdef(CONFIG_PLANOSZ_BACKEND_ESP32 backends/esp32/esp32_backend.c)
endif()
