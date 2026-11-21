# Linux convenience targets for the substitute builds.
#
# These builds are for playing and porting. None is byte-matching or
# evidence for config/functions/ status; the matching build is game.cmd
# with the pinned VC5 toolchain on Windows. See workflow/docs/clang_build.md,
# workflow/docs/vc6_wine_build.md and workflow/docs/asm_port.md.

PYTHON    ?= build/linux-venv/bin/python
VC6_DIR   ?= /home/ubuntu/aoe_web/MSVC600
WINE      ?= wine
PLAY_DIR  ?= build/play
# Local substitute configs for the Wine builds (see vc6_wine_build.md). The
# assembler is used only by the harness, for its assembly reference.
VC6_TOOLCHAIN ?= build/toolchain-vc6.json
ASSEMBLER     ?= build/assembler-ml615.json

# The VC6 tree as Wine sees it (Z: maps to /).
VC6_WIN   = Z:$(subst /,\,$(VC6_DIR))
WINE_ENV  = VC5_DIR='$(VC6_WIN)' AOE_TOOLCHAIN=$(VC6_TOOLCHAIN) AOE_ASSEMBLER=$(ASSEMBLER)
LATEST_CLANG = $$(ls -d build/clang/2*/ 2>/dev/null | tail -1)

.PHONY: all help clang vc6 harness test play clean

all: clang

help:
	@echo "make clang      native clang-cl/lld-link build with SDL2 (default) -> build/clang/<time>/empiresx.exe + SDL2.dll"
	@echo "make vc6      VC6 under Wine                               -> build/executable/vc6/<time>/"
	@echo "make harness    check the C++ ports against the assembly (VC6 + ml.exe under Wine)"
	@echo "make test       tool tests for the port harness and build scripts"
	@echo "make play       run the newest clang build under Wine with GAMECD (needs runtime/ data)"
	@echo "make clean     remove build/clang and build/asm_port"
	@echo "Variables: PYTHON=$(PYTHON) VC6_DIR=$(VC6_DIR) WINE=$(WINE) PLAY_DIR=$(PLAY_DIR)"

clang:
	VC6_DIR=$(VC6_DIR) $(PYTHON) tools/build_clang.py

vc6:
	$(WINE_ENV) $(PYTHON) tools/build_executable.py --output-dir build/executable/vc6 --no-icon

harness:
	$(WINE_ENV) $(PYTHON) tools/asm_port_harness.py

test:
	$(PYTHON) -m pytest -q tests/test_asm_port_harness.py tests/test_build_executable.py tests/test_drawing_binary.py

# Copies runtime/ (the retail install) once, then drops in the newest clang build.
play:
	@test -n "$(LATEST_CLANG)" || { echo "No clang build yet: run make clang"; exit 1; }
	@ls runtime | grep -qiv '^readme' || { echo "runtime/ has no game data: see runtime/README.md"; exit 1; }
	@test -d $(PLAY_DIR) || cp -r runtime $(PLAY_DIR)
	cp "$(LATEST_CLANG)empiresx.exe" "$(LATEST_CLANG)SDL2.dll" $(PLAY_DIR)/
	cd $(PLAY_DIR) && $(WINE) empiresx.exe NOSTARTUP GAMECD

clean:
	rm -rf build/clang build/asm_port
