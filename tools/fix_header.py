#!/usr/bin/env python3
"""
fix_header.py — patch a GBA ROM's header complement check in place.

The BIOS validates a checksum over header bytes 0xA0..0xBC and refuses to
boot if byte 0xBD doesn't match. devkitPro's `gbafix` does this (along with
inserting Nintendo's logo bitmap, which this script does NOT and cannot do —
that data isn't ours to ship). Computing the checksum needs nothing from
Nintendo, so it's worth doing here regardless: it removes one of the two
reasons the ROM wouldn't boot on hardware, and costs nothing.

Usage: python3 tools/fix_header.py build/tengen.gba
"""
import sys


def complement_check(header: bytes) -> int:
    """Checksum over ROM bytes 0xA0..0xBC, per the GBA cartridge header spec."""
    value = 0
    for byte in header[0xA0:0xBD]:
        value = (value - byte) & 0xFF
    return (value - 0x19) & 0xFF


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip())
        return 2

    path = sys.argv[1]
    with open(path, "rb") as fh:
        rom = bytearray(fh.read())

    if len(rom) < 0xC0:
        print(f"{path}: too small to contain a GBA header ({len(rom)} bytes)")
        return 1

    expected = complement_check(rom)
    if rom[0xBD] == expected:
        print(f"{path}: header checksum already correct (0x{expected:02X})")
        return 0

    rom[0xBD] = expected
    with open(path, "wb") as fh:
        fh.write(rom)
    print(f"{path}: header checksum set to 0x{expected:02X}")

    if all(b == 0 for b in rom[4:0xA0]):
        print("  note: the Nintendo logo area is still zeroed, so this ROM runs "
              "in emulators but will not boot on real hardware. Run devkitPro's "
              "gbafix to fill it in.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
