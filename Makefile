# Tengen NES Tetris -> GBA port
#
# Two build modes:
#
#   make / make test   Native host build of the platform-independent core
#                       plus its unit tests. Needs only gcc. This is the
#                       day-to-day loop: the core is developed and verified
#                       against the disassembly here, on the host, before it
#                       ever touches a GBA.
#
#   make gba            Cross-compiles the core (unmodified) into a .gba ROM
#                       once the GBA-side rendering/input layer exists under
#                       gba/. Requires devkitARM (devkitPro) with DEVKITARM
#                       set. This target is a stub until that layer is
#                       written — see CLAUDE.md's roadmap.

CC      ?= gcc
CFLAGS  ?= -std=c99 -Wall -Wextra -Wpedantic -g -O1
BUILD   := build

CORE_SRC := src/tengen_core.c
CORE_HDR := src/tengen_core.h
TEST_SRC := tests/test_tengen.c
TEST_BIN := $(BUILD)/test_tengen

.PHONY: all test clean gba

all: test

$(BUILD):
	mkdir -p $(BUILD)

$(TEST_BIN): $(CORE_SRC) $(CORE_HDR) $(TEST_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(CORE_SRC) $(TEST_SRC) -o $(TEST_BIN)

test: $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -rf $(BUILD)

# ---------------------------------------------------------------------------
# GBA cross-build (stub)
# ---------------------------------------------------------------------------
ifdef DEVKITARM
GBAROM := $(BUILD)/tengen.gba
gba: $(GBAROM)
$(GBAROM):
	@echo "GBA build not wired up yet: gba/ (rendering, input, audio) doesn't" \
	      "exist. See CLAUDE.md's roadmap for the next step. tengen_core.c" \
	      "itself is plain C99 and will compile as-is under" \
	      "$(DEVKITARM)/bin/arm-none-eabi-gcc once there's a main.c to link" \
	      "it against."
	@exit 1
else
gba:
	@echo "DEVKITARM is not set. Install devkitPro/devkitARM and re-run," \
	      "e.g.: DEVKITARM=/opt/devkitpro/devkitARM make gba"
	@exit 1
endif
