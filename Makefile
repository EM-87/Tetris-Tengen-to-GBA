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
# The resulting ROM runs in emulators (mGBA, VBA-M, no$gba). It will NOT boot
# on real hardware until its header carries Nintendo's logo data and a valid
# checksum — run devkitPro's `gbafix` over the .gba to fix that up. See
# gba/crt0.s for the details.
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

GBA_SRC  := gba/main.c $(CORE_SRC)
GBA_ELF  := $(BUILD)/tengen.elf
GBA_ROM  := $(BUILD)/tengen.gba

gba: $(GBA_ROM)

$(GBA_ELF): $(GBA_SRC) $(CORE_HDR) gba/gba_hw.h gba/crt0.s gba/gba.ld | $(BUILD)
	@command -v $(ARMCC) >/dev/null 2>&1 || { \
	    echo "error: $(ARMCC) not found."; \
	    echo "Install an arm-none-eabi toolchain (see the Makefile header),"; \
	    echo "or point DEVKITARM at a devkitARM install."; \
	    exit 1; }
	$(ARMCC) $(ARMFLAGS) $(ARMLDFLAGS) -marm gba/crt0.s $(GBA_SRC) -o $@

$(GBA_ROM): $(GBA_ELF)
	$(ARMOBJCOPY) -O binary $< $@
	@python3 tools/fix_header.py $@ 2>/dev/null || \
	    echo "note: python3 unavailable, header checksum left unpatched"
	@echo "Built $@ ($$(wc -c < $@) bytes)"

# Boots the ROM in mGBA headlessly and checks it actually renders and plays,
# rather than merely linking. Needs: pip install pygba (plus the mGBA shared
# library, e.g. apt-get install libmgba0.10).
.PHONY: gba-check
gba-check: $(GBA_ROM)
	python3 tools/run_rom.py $(GBA_ROM) --selftest
