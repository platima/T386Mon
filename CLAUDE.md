# T386Mon: notes for AI assistants

## What this is

T386MON is a performance monitor and benchmark for Windows 95 on tiny386 (github.com/hchunhui/tiny386): one Win32 program, `T386MON.EXE`, shipped on a 1.44 MB floppy image, `t386mon.img`. It reads the emulator's own counters through tiny386's information port (perfport: I/O ports 0F1F8h to 0F1FFh, `PROTOCOL.md`, `perfport.h`), and shows Windows' own figures from `HKEY_DYN_DATA\PerfStats` and `GlobalMemoryStatus`. Without the port (older firmware, upstream tiny386, any other PC) it still runs and shows Windows' figures only. The port exists in Plat's bare-metal ESP32-S31 tiny386 firmware from v4.3 on, not upstream.

## Layout

- `src/t386mon.c` - the whole program, one C file with its own tiny runtime (no C library)
- `src/t386mon.rc`, `src/mkicon.py` - version resource and the icon (drawn by the script, no image files)
- `src/pecheck.py`, `src/cpucheck.py`, `src/w95imports.txt` - the checks `build.sh` runs after the link
- `src/README.TXT` - the floppy's short guide (converted to DOS line ends at build time)
- `build.sh` - builds `T386MON.EXE` and `t386mon.img` into `out/` (or the folder given)
- `perfport.h`, `PROTOCOL.md` - the information port's protocol and what each value means
- `port-test/` - PPTEST.COM (DOS, GNU as), its build script and its log checker
- `.github/workflows/ci.yml` - build and checks on every push; `release.yml` - the release on a tag

## Build and test

- Debian 12 (bookworm), WSL is fine: `gcc-mingw-w64-i686-win32 binutils-mingw-w64-i686 mingw-w64-i686-dev mtools python3 binutils`.
- `bash build.sh [OUT]` builds and checks; `pecheck.py` and `cpucheck.py` failing stops the build. Zero compiler warnings with `-Wall -Wextra` is the standard.
- `bash port-test/build.sh [OUT]` builds PPTEST.COM.
- The build is reproducible (`SOURCE_DATE_EPOCH` is fixed in `build.sh`): two builds into different folders must give identical files. CI checks this. Only a source change may change the hashes, never the toolchain or the folder.
- CI runs in a `debian:12` container with the toolchain's package versions pinned in `ci.yml`. If Debian replaces one, update the pin and check that the output is still the same bytes.
- There is no runtime test in this repo. T386MON was tested in Windows 95 on Plat's PC harness (the firmware's emulator core on a PC, outside this repo) and is tested on the board by Plat by hand. Say which one a figure comes from.

## Constraints that must hold

- **Target**: Windows 95 OSR2 on tiny386, which emulates a 386 with no FPU (`fpu = 0`). Integer code only: no `float` or `double`, no x87, no MMX or SSE, nothing the 486 or later added. `cpucheck.py` enforces it. No 64-bit division (nothing from libgcc is linked); use the 386's own `div` or `MulDiv`.
- **No C library and no newer APIs**: imports from KERNEL32, USER32, GDI32 and ADVAPI32 only, ANSI (`A`) functions only, and only those listed in `src/w95imports.txt`, each checked to exist in Windows 95 OSR2's own DLLs. A new import needs that check first (not Windows 98 or NT 4), then a line in `w95imports.txt`.
- **The PE must load on Windows 95**: OS and subsystem version 4.0, image base 400000h, no relocations, TLS or load configuration (the linker flags in `build.sh`, checked by `pecheck.py`).
- **8.3 names** for everything that goes onto the floppy or into Windows: `T386MON.EXE`, `README.TXT`, `C:\T386MON.LOG`.
- **The information port is a versioned protocol shared with the firmware.** `perfport.h` here must stay the same bytes as the firmware's copy. Never change what an existing index means; new values take a free index with a minor version bump, made in the firmware first. T386MON checks valid bits, so it must keep working with port 1.0 and with no port at all.
- **Cost matters**: at 800x480 with VMDisp9x each GDI call costs about 20 ms of guest time, so the once-a-second work stays small, and nothing writes to the disk during a test (each write leads to a flush to the SD card on the board).
- **Versions move together**: `APP_VERSION` in `src/t386mon.c`, `FILEVERSION`, `PRODUCTVERSION` and the version strings in `src/t386mon.rc`, and the release tag (`v1.2` for 1.2; `release.yml` refuses a tag that doesn't match).

## Never commit

- Built files: `T386MON.EXE`, `t386mon.img`, `PPTEST.COM`, `out/`. They belong in releases and CI artifacts.
- Anything of Windows: system files, DLLs, export lists made from them, disk images, CD or floppy images of Windows, screenshots that show more of Windows than T386MON's own window, and logs read out of someone's Windows image.
- Display driver binaries (VMDisp9x), game data, or anything from Plat's SD card.

Check `git status` and `git diff --cached --stat` before every commit.

## Writing style

- Australian English (colour, licence, behaviour, maximise). No em dashes and no double hyphens in prose; use a spaced hyphen ( - ) where a dash would go.
- `README.md` follows Plat's npad pattern: the visitor badge line first, a title with an emoji, an emoji on each section heading, first person as Plat, and the closing sections Sponsor This Project, Acknowledgements, Licence, Author.
- Plain and concrete. Say what was measured and where (PC harness or board); don't claim board results that haven't been seen.

## Commits

- Author Platima, as git is configured here. End each commit message with:
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`
- Contributions to upstream projects (tiny386 itself) are different: they go out as Plat alone, with no AI co-author or generated-with lines. Nothing in this repo is sent upstream.
