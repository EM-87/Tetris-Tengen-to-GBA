# gbafix (vendored)

`gbafix.c` here is devkitPro's own ROM header tool, taken unmodified from
their `gba-tools` repository. It is LGPL v2.1-or-later; its licence header is
intact at the top of the file.

## Why it is in this repo

A GBA cartridge header has to carry two things this build cannot compute on
its own before real hardware will boot the ROM:

- **The boot logo** (bytes 0x04-0x9F). The console's BIOS compares it against
  its own copy and refuses to run anything that does not match. It is a fixed
  interoperability constant, not something a program can derive.
- **The complement check** (byte 0xBD), a checksum over the header.

`gbafix` supplies both. It is the standard tool every GBA homebrew project
uses for exactly this, which is why it is vendored rather than reimplemented:
a hand-rolled substitute would have to embed the same logo data with none of
the provenance.

The build compiles it with the host compiler and runs it over the linked ROM;
see the `gba` target in the Makefile. If you already have devkitPro
installed, its `gbafix` is the same program.

Upstream: https://github.com/devkitPro/gba-tools
