#!/bin/bash
# build.sh: T386MON.EXE and its floppy image, t386mon.img.
#
#   bash build.sh [OUT]          (OUT: default ./out)
#
# Needs Debian 12 (bookworm) or the like, in WSL or not, with
#   gcc-mingw-w64-i686-win32 binutils-mingw-w64-i686 mingw-w64-i686-dev
#   mtools python3
# (apt-get install them, or apt-get download and dpkg -x them anywhere and
# put their usr/bin on PATH).  Nothing from the C library is linked:
# Windows 95 has no MSVCRT.DLL, so t386mon.c has its own few helpers and
# imports from KERNEL32, USER32, GDI32 and ADVAPI32 only.  perfport.h, the
# information port's protocol, is the one beside this script.
#
# The checks after the link (each stops the build if it fails):
#   pecheck.py   a PE Windows 95 loads: version 4.0, no TLS or the like,
#                writable imports, and every imported function in
#                src/w95imports.txt (each one checked against Windows 95
#                OSR2's own DLLs; with W95REF=DIR, a folder of DLL.exports
#                lists made from those DLLs, it checks them all again)
#   cpucheck.py  386 integer instructions only: no x87 (tiny386 runs
#                Windows with fpu = 0), no MMX or SSE, nothing the 486 added
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/src
OUT=${1:-$HERE/out}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
P=i686-w64-mingw32
CC=$P-gcc
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1792713600}   # 2026-10-23 00:00 UTC

CFLAGS=(-std=gnu99 -Os -march=i386 -mtune=i386 -mno-80387 -mno-mmx -mno-sse -mno-sse2
        -mno-3dnow -mno-stack-arg-probe -mpreferred-stack-boundary=2 -ffreestanding
        -fno-builtin -fno-tree-loop-distribute-patterns -fno-stack-protector
        -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident -Wa,-march=i386
        -Wall -Wextra)
LDFLAGS=(-nostdlib -nostartfiles -s -Wl,--entry,_entry -Wl,--subsystem,windows:4.0
         -Wl,--major-os-version,4 -Wl,--minor-os-version,0
         -Wl,--major-image-version,1 -Wl,--minor-image-version,0
         -Wl,--disable-dynamicbase -Wl,--disable-nxcompat -Wl,--disable-reloc-section
         -Wl,--no-insert-timestamp -Wl,--file-alignment,512 -Wl,--section-alignment,4096
         -Wl,--image-base,0x400000)

echo "== $($CC --version | head -1)"
python3 -I "$SRC/mkicon.py" "$OUT/t386mon.ico"
$P-windres -I "$OUT" -I "$SRC" "$SRC/t386mon.rc" -O coff -o "$OUT/res.o"
$CC "${CFLAGS[@]}" -I "$SRC" -I "$HERE" -c "$SRC/t386mon.c" -o "$OUT/t386mon.o" 2> "$OUT/warnings.txt" || {
    cat "$OUT/warnings.txt"; exit 1; }
echo "== compiler warnings: $(grep -c 'warning:' "$OUT/warnings.txt" || true)"
cat "$OUT/warnings.txt"
$CC "${LDFLAGS[@]}" -o "$OUT/T386MON.EXE" "$OUT/t386mon.o" "$OUT/res.o" \
    -lkernel32 -luser32 -lgdi32 -ladvapi32
ls -l "$OUT/T386MON.EXE"

echo "== pecheck"
CODE=$($P-objdump -h "$OUT/t386mon.o" | awk '$2 == ".text" {print $3}')
python3 -I "$SRC/pecheck.py" --code "$CODE" "$OUT/T386MON.EXE" "$SRC/w95imports.txt" \
    ${W95REF:+"$W95REF"} | tee "$OUT/pecheck.txt"
echo "== cpucheck (the object's code: all the program has)"
$P-objdump -d "$OUT/t386mon.o" > "$OUT/disasm.txt"
python3 -I "$SRC/cpucheck.py" "$OUT/disasm.txt" | tee "$OUT/cpucheck.txt"

echo "== the floppy image"
STAMP=$(date -u -d "@$SOURCE_DATE_EPOCH" '+%Y-%m-%d %H:%M:%S')
sed 's/\r*$/\r/' "$SRC/README.TXT" > "$OUT/README.TXT"      # DOS line ends
touch -d "$STAMP" "$OUT/T386MON.EXE" "$OUT/README.TXT"
rm -f "$OUT/t386mon.img"
export MTOOLS_SKIP_CHECK=1
mformat -C -f 1440 -v T386MON -N 38633354 -i "$OUT/t386mon.img" ::
mcopy -m -i "$OUT/t386mon.img" "$OUT/T386MON.EXE" ::T386MON.EXE
mcopy -m -i "$OUT/t386mon.img" "$OUT/README.TXT" ::README.TXT
mdir -i "$OUT/t386mon.img" ::
ls -l "$OUT/t386mon.img"
(cd "$OUT" && sha256sum T386MON.EXE t386mon.img)
