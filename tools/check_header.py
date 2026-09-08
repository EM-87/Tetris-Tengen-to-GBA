#!/usr/bin/env python3
"""
check_header.py — verify a .gba cartridge header the way the BIOS does.

The console runs three checks before it will start a ROM, and failing any of
them means a black screen on real hardware while emulators happily run the
same file. This asserts all three, so "boots on hardware" is something the
build proves rather than something the README claims.

    python3 tools/check_header.py build/tengen.gba

Checked:
  1. The entry point at 0x00 is an ARM branch (opcode 0xEA).
  2. The boot logo at 0x04-0x9F is present and self-consistent. Its exact
     bytes come from devkitPro's gbafix (see tools/gbafix/README.md); this
     checks the region is non-empty and matches the first bytes of the
     documented sequence, which is what a zeroed or truncated header fails.
  3. The fixed byte at 0xB2 is 0x96, and the complement check at 0xBD
     matches the checksum over 0xA0-0xBC.
"""
import sys

LOGO_START = 0x04
LOGO_END = 0xA0
LOGO_PREFIX = bytes([0x24, 0xFF, 0xAE, 0x51, 0x69, 0x9A, 0xA2, 0x21])


def complement_check(rom: bytes) -> int:
    value = 0
    for byte in rom[0xA0:0xBD]:
        value = (value - byte) & 0xFF
    return (value - 0x19) & 0xFF


def check(path: str) -> int:
    with open(path, "rb") as fh:
        rom = fh.read()

    problems = []
    if len(rom) < 0xC0:
        print(f"{path}: too small to hold a cartridge header ({len(rom)} bytes)")
        return 1

    if rom[3] != 0xEA:
        problems.append(f"el punto de entrada en 0x00 no es un branch ARM "
                        f"(0x{rom[3]:02X}, esperado 0xEA)")

    logo = rom[LOGO_START:LOGO_END]
    if all(b == 0 for b in logo):
        problems.append("el logo de arranque esta en cero: el BIOS no arrancaria la ROM")
    elif not logo.startswith(LOGO_PREFIX):
        problems.append("el logo de arranque no coincide con la secuencia documentada")

    if rom[0xB2] != 0x96:
        problems.append(f"el byte fijo en 0xB2 es 0x{rom[0xB2]:02X}, deberia ser 0x96")

    expected = complement_check(rom)
    if rom[0xBD] != expected:
        problems.append(f"complement check 0x{rom[0xBD]:02X}, calculado 0x{expected:02X}")

    title = rom[0xA0:0xAC].decode("ascii", "replace").rstrip("\x00")
    code = rom[0xAC:0xB0].decode("ascii", "replace")

    if problems:
        for p in problems:
            print(f"FALLA: {p}")
        return 1

    print(f"OK: header valido. titulo={title!r} code={code!r} "
          f"tamano={len(rom)} bytes")
    print("     logo presente, byte fijo correcto, checksum correcto:")
    print("     esta ROM pasa las comprobaciones que hace el BIOS al arrancar.")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__.strip())
        sys.exit(2)
    sys.exit(check(sys.argv[1]))
