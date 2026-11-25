# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# The repository root is a Zephyr module (zephyr/module.yml); pull it in without needing
# ZEPHYR_EXTRA_MODULES on the command line.
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_LIST_DIR}/../..)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(planosz_node)

target_sources(app PRIVATE src/main.c src/demo_plans.c)
target_include_directories(app PRIVATE src)
