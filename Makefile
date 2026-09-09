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

CORE_SRC := src/tengen_core.c src/tengen_link.c
CORE_HDR := src/tengen_core.h src/tengen_link.h
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
# GBA cross-build
#
# Uses a stock arm-none-eabi toolchain rather than requiring devkitARM: the
# GBA-side code depends only on hardware registers it declares itself (see
# gba/gba_hw.h), plus a hand-written crt0 and linker script. If DEVKITARM is
# set, its toolchain is used instead — same sources either way.
#
# Install the toolchain with e.g.:
#   apt-get install gcc-arm-none-eabi        (Debian/Ubuntu)
#   brew install --cask gcc-arm-embedded     (macOS)
#
# The resulting ROM boots on real hardware: the build finishes by running
# devkitPro's own `gbafix` (vendored in tools/gbafix/, LGPL — see the README
# there) over the linked binary, which stamps the cartridge header's boot logo
# and complement check. Without that step the BIOS refuses to start the ROM.
# ---------------------------------------------------------------------------
ifdef DEVKITARM
ARMPREFIX := $(DEVKITARM)/bin/arm-none-eabi-
else
ARMPREFIX := arm-none-eabi-
endif

ARMCC      := $(ARMPREFIX)gcc
ARMOBJCOPY := $(ARMPREFIX)objcopy

# ARM7TDMI. -mthumb keeps the code small, which matters because it runs from
# the 16-bit-wide cartridge bus.
ARMFLAGS := -mcpu=arm7tdmi -mtune=arm7tdmi -mthumb -mthumb-interwork \
            -std=c99 -Wall -Wextra -O2 -ffreestanding -fno-builtin \
            -fomit-frame-pointer
# -nostartfiles (not -nostdlib): crt0.s replaces the toolchain's startup, but
# libc and libgcc are still wanted — the core calls memcpy/memset, and the
# 32-bit division in the score wrap needs libgcc's helper. Nothing here pulls
# in newlib's syscall-backed side (no malloc, no stdio), so no stubs needed.
ARMLDFLAGS := -T gba/gba.ld -nostartfiles -Wl,--gc-sections

# Graphics headers built from a cartridge dump by `make assets`. They are not
# in version control (the artwork is the original game's), so the ROM target
# checks for them and says what to do rather than failing in the compiler.
GBA_ASSETS := gba/tiles_game.h gba/screen_1p.h gba/palettes_rom.h gba/audio_prg.h

GBA_SRC  := gba/main.c gba/nes_audio.c gba/nes6502.c gba/link.c $(CORE_SRC)
GBA_ELF  := $(BUILD)/tengen.elf
GBA_ROM  := $(BUILD)/tengen.gba

gba: $(GBA_ROM)

# The generated headers are prerequisites through $(wildcard) rather than
# directly: that way regenerating assets rebuilds the ROM, while a missing one
# still reaches the helpful message below instead of "no rule to make target".
$(GBA_ELF): $(GBA_SRC) $(CORE_HDR) gba/gba_hw.h gba/link.h gba/crt0.s gba/gba.ld \
             $(wildcard $(GBA_ASSETS)) | $(BUILD)
	@command -v $(ARMCC) >/dev/null 2>&1 || { \
	    echo "error: $(ARMCC) not found."; \
	    echo "Install an arm-none-eabi toolchain (see the Makefile header),"; \
	    echo "or point DEVKITARM at a devkitARM install."; \
	    exit 1; }
	@for h in $(GBA_ASSETS); do test -f $$h || { \
	    echo "error: $$h is missing."; \
	    echo "The port's graphics come from an original cartridge dump and are"; \
	    echo "not committed here. Generate them once with:"; \
	    echo "    make assets ROM=/path/to/tetris.nes"; \
	    exit 1; }; done
	$(ARMCC) $(ARMFLAGS) $(ARMLDFLAGS) -marm gba/crt0.s $(GBA_SRC) -o $@

GBAFIX := $(BUILD)/gbafix

$(GBAFIX): tools/gbafix/gbafix.c | $(BUILD)
	$(CC) -O2 -w -o $@ $<

$(GBA_ROM): $(GBA_ELF) $(GBAFIX)
	$(ARMOBJCOPY) -O binary $< $@
	$(GBAFIX) $@ -tTENGENTETRIS -cCTTE -m00 -p
	@echo "Built $@ ($$(wc -c < $@) bytes) - header fixed, boots on hardware"

# Builds the port's graphics from an original cartridge dump: tiles, palettes
# and the screen layout, all taken from the ROM rather than redrawn. Nothing
# here ships game artwork; you supply the dump.
#   make assets ROM=/path/to/tetris.nes
.PHONY: assets
assets:
	@test -n "$(ROM)" || { echo "usage: make assets ROM=/path/to/tetris.nes"; exit 1; }
	python3 tools/extract_assets.py "$(ROM)" -o gba/

# Verifies the asset conversion's format handling without needing a ROM.
.PHONY: assets-check
assets-check:
	python3 tools/extract_assets.py --self-test

# Boots the ROM in mGBA headlessly and checks it actually renders and plays,
# rather than merely linking. The last two are the two-player mode: one console
# on its own reaching the link screen and giving up gracefully, then TWO cores
# with a simulated cable between them playing the same match byte for byte.
# Needs: pip install pygba (plus the mGBA shared library, e.g. apt-get install
# libmgba0.10).
.PHONY: gba-check
gba-check: $(GBA_ROM)
	python3 tools/check_header.py $(GBA_ROM)
	python3 tools/run_rom.py $(GBA_ROM) --selftest
	python3 tools/run_rom.py $(GBA_ROM) --lineclear
	python3 tools/run_rom.py $(GBA_ROM) --pause
	python3 tools/run_rom.py $(GBA_ROM) --audio
	python3 tools/run_rom.py $(GBA_ROM) --pause-audio
	python3 tools/run_rom.py $(GBA_ROM) --title
	python3 tools/run_rom.py $(GBA_ROM) --link
	python3 tools/run_link.py $(GBA_ROM)
