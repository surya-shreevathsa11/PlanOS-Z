# PlanOS-Z developer entry points. Everything heavy lives in gitignored dirs
# (.venv, .zephyr-ws, .zephyr-sdk, build/); `make distclean` removes all of it.

SHELL   := /bin/bash
PY      ?= python3
VENV    := .venv
BIN     := $(VENV)/bin
PLANOSZC := $(BIN)/planoszc
MISSIONS := examples/missions
APPSRC  := apps/planosz_node/src

.PHONY: help install test demo demo-json codegen app-plans golden \
        zephyr-setup native-sim native-sim-demo esp32-build clean distclean

help:
	@echo "host:    make install | test | demo | demo-json | codegen | app-plans | golden"
	@echo "zephyr:  make zephyr-setup | native-sim | native-sim-demo | esp32-build"
	@echo "clean:   make clean | distclean"

$(BIN)/pip:
	$(PY) -m venv $(VENV)

install: $(BIN)/pip
	$(BIN)/pip install -q -e "tools/planoszc[dev]"

test: install
	$(BIN)/python -m pytest tests/host -q

demo: install
	-$(PLANOSZC) admit $(MISSIONS)/01_parallel_sense_accept.yaml
	-$(PLANOSZC) admit $(MISSIONS)/02_deadline_reject.yaml
	-$(PLANOSZC) admit $(MISSIONS)/03_resource_conflict_reject.yaml
	$(PLANOSZC) run $(MISSIONS)/01_parallel_sense_accept.yaml

demo-json: install
	$(PLANOSZC) run $(MISSIONS)/01_parallel_sense_accept.yaml --format ndjson
	-$(PLANOSZC) admit $(MISSIONS)/02_deadline_reject.yaml --format json

# IDL -> C registry table (tests fail if the committed copy is stale)
codegen: install
	$(PLANOSZC) codegen

# Missions -> IR headers linked into the Zephyr app (reject plans use --force so the
# device can prove it re-rejects them).
app-plans: install
	$(PLANOSZC) compile $(MISSIONS)/01_parallel_sense_accept.yaml --emit c --symbol PLAN_ACCEPT -o $(APPSRC)/plan_accept.gen.h
	$(PLANOSZC) compile $(MISSIONS)/02_deadline_reject.yaml --emit c --force --symbol PLAN_DEADLINE -o $(APPSRC)/plan_deadline.gen.h
	$(PLANOSZC) compile $(MISSIONS)/03_resource_conflict_reject.yaml --emit c --force --symbol PLAN_CONFLICT -o $(APPSRC)/plan_conflict.gen.h

# Regenerate the golden trace (only if a behaviour change is intentional)
golden: install
	mkdir -p traces/golden
	$(PLANOSZC) run $(MISSIONS)/01_parallel_sense_accept.yaml --format ndjson > traces/golden/parallel_sense_accept.ndjson

# ---- Zephyr ---------------------------------------------------------------------
zephyr-setup:
	tools/zephyr_setup.sh

native-sim:
	. tools/zephyr_env.sh && cd .zephyr-ws && west build -b native_sim/native/64 -d $(CURDIR)/build/native_sim $(CURDIR)/apps/planosz_node -p auto
	@echo "Interactive shell: build/native_sim/zephyr/zephyr.exe  (then: planosz plans, planosz run accept)"

native-sim-demo:
	. tools/zephyr_env.sh && cd .zephyr-ws && west build -b native_sim/native/64 -d $(CURDIR)/build/native_sim_demo $(CURDIR)/apps/planosz_node -p auto -- -DEXTRA_CONF_FILE=demo.conf
	build/native_sim_demo/zephyr/zephyr.exe < /dev/null

esp32-build:
	. tools/zephyr_env.sh && cd .zephyr-ws && west build -b esp32_devkitc/esp32/procpu -d $(CURDIR)/build/esp32 $(CURDIR)/apps/planosz_node -p auto
	@echo "Flash: . tools/zephyr_env.sh && west flash -d build/esp32   (needs an ESP32 board + USB)"

clean:
	rm -rf build .pytest_cache
	find . -name __pycache__ -not -path "./.venv/*" -not -path "./.zephyr-ws/*" -prune -exec rm -rf {} +
	rm -rf tools/planoszc/*.egg-info

distclean: clean
	rm -rf $(VENV) .zephyr-ws .zephyr-sdk .zephyr-sdk-dl
