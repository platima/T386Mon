---
applyTo: '**'
---
## Project overview
You are assisting with **T386MON**, a performance monitor and benchmark for Windows 95 running on tiny386 (an x86 PC emulator, github.com/hchunhui/tiny386). It is one Win32 program, `T386MON.EXE`, shipped on a 1.44 MB floppy image, `t386mon.img`. It reads the emulator's own counters through tiny386's information port (I/O ports 0F1F8h to 0F1FFh, described in `PROTOCOL.md` and `perfport.h`) and Windows 95's own performance data. Without the port it still runs on any Windows 95 PC and shows Windows' figures only. The port is in Plat's bare-metal ESP32-S31 tiny386 firmware from v4.3 on, not in upstream tiny386.

## Layout
- `src/t386mon.c`: the whole program, one C file with its own small runtime (no C library)
- `src/t386mon.rc`, `src/mkicon.py`: version resource and icon (drawn by the script)
- `src/pecheck.py`, `src/cpucheck.py`, `src/w95imports.txt`: the checks run after the link
- `src/README.TXT`: the floppy's short guide
- `build.sh`: builds `T386MON.EXE` and `t386mon.img` into `out/`
- `perfport.h`, `PROTOCOL.md`: the information port's protocol
- `port-test/`: PPTEST.COM (DOS, GNU as), its build script and log checker
- `.github/workflows/`: `ci.yml` on every push, `release.yml` on a `v*` tag

## Build and test
- Debian 12 with `gcc-mingw-w64-i686-win32 binutils-mingw-w64-i686 mingw-w64-i686-dev mtools python3 binutils`.
- `bash build.sh [OUT]`; `pecheck.py` or `cpucheck.py` failing stops the build. Keep zero warnings with `-Wall -Wextra`.
- `bash port-test/build.sh [OUT]` for PPTEST.COM.
- The build is reproducible: two builds in different folders give identical bytes. CI (a `debian:12` container with pinned package versions) checks this.
- There is no automated runtime test; the program is tested in Windows 95 on a PC harness and on the board by hand. Say which a figure comes from.

## Hard constraints
- Windows 95 OSR2 on an emulated 386 with **no FPU**: integer code only, no `float` or `double`, no x87, MMX, SSE or 486-and-later instructions, no 64-bit division helpers.
- **No C library** (Windows 95 has no MSVCRT.DLL) and **no API newer than Windows 95**: imports from KERNEL32, USER32, GDI32 and ADVAPI32 only, ANSI functions only, each listed in `src/w95imports.txt` after being checked against Windows 95 OSR2's own DLLs.
- PE for Windows 95: OS and subsystem version 4.0, image base 400000h, no relocations or TLS.
- **8.3 file names** for anything on the floppy or in Windows.
- The information port protocol is **versioned and shared with the firmware**: `perfport.h` stays byte-identical to the firmware's copy, an index never changes meaning, new values come with a minor version from the firmware side, and T386MON must keep working with port 1.0 or no port.
- Drawing is expensive on the target (about 20 ms of guest time a GDI call at 800x480), so keep the periodic work small and never write to the disk during a test.
- `APP_VERSION` in `src/t386mon.c`, the versions in `src/t386mon.rc` and the release tag move together.

## Never commit
- Built binaries or images (`T386MON.EXE`, `t386mon.img`, `PPTEST.COM`, `out/`): they go in releases and CI artifacts.
- Anything from Windows (system files, DLLs, export lists made from them, disk or install images, logs read from someone's Windows image, screenshots showing more of Windows than T386MON's window), display driver binaries, or game data.

## When providing assistance
- Use Australian English. No em dashes and no double hyphens in prose; a spaced hyphen ( - ) where a dash would go.
- `README.md` follows the npad pattern: visitor badge first, title with an emoji, emoji on section headings, first person as Plat, closing with Sponsor This Project, Acknowledgements, Licence and Author.
- Commits are authored by Platima as git is configured; when Claude helped, the message ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Contributions to upstream tiny386 go out as Plat alone, with no AI lines.
- Prefer the smallest change that keeps the program cheap to run in the guest.
