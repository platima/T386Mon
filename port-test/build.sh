#!/bin/bash
# port-test/build.sh: PPTEST.COM, the information port's test for DOS.
#
#   bash port-test/build.sh [OUT]     (OUT: default ./out beside build.sh)
#
# Needs GNU as and ld that make 32-bit x86 code (Debian's binutils on a PC
# does).  PPTEST.COM says what it reads on the BIOS's debug port, 0402h, a
# line at a time; the PC harness logs that port, and ppcheck.py checks the
# log.  bios.inc is the helper file of my tiny386 BIOS tests, unchanged.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/../out}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

echo "== $(as --version | head -1)"
as --32 -I "$HERE" -o "$OUT/pptest.o" "$HERE/pptest.S"
ld -m elf_i386 -Ttext 0x100 --oformat binary -o "$OUT/PPTEST.COM" "$OUT/pptest.o"
ls -l "$OUT/PPTEST.COM"
(cd "$OUT" && sha256sum PPTEST.COM)
