# The tiny386 information port (perfport), version 1.1

For T386MON and for any other program inside the emulated PC that wants to know how fast tiny386 is running it. The protocol itself is [`perfport.h`](perfport.h); this note says what each value means, how to use them, how the port was chosen and checked, and what only a board can say.

Claims are marked [verified] (seen in a test or a run), [inferred] (worked out, not seen) or [unknown]. "The PC harness" is the board firmware's emulator core built to run on a PC, with a virtual clock of 2 million guest instructions a second, as on the board.

Version 1.0 and 1.1 were written on 2026-10-23; 1.1 added the pictures that changed (`CHANGED`, `CHANGED_1S`), the panel's real frames a second, and mended the last whole second after a pause. The port is in my bare-metal ESP32-S31 tiny386 firmware from v4.3 (2026-10-24) on, with this protocol unchanged.


## 1. In short

* **In the bare-metal ESP32-S31 firmware v4.3 and later.** Firmware before v4.3, and upstream tiny386, have no port (section 2 says what a program reads there).
* Eight I/O ports, 0F1F8h to 0F1FFh: write an index, read a 32-bit value. One command takes a snapshot, so that values read one after another belong together. Reads change nothing in the emulator or the guest [verified: section 9].
* 128 indexes: the emulator's identity and clocks, the guest's instructions and how much of the time it is halted, every picture the board draws and how long it took, and (1.1) those that changed, the rows sent to the panel, the VBE drawing's blocks and rows, the display mode, memory, the disks, the guest's timer, the keyboard and mouse, the log, the status LED, and two strings (the firmware's version and the board's name) [verified: host tests, PC harness runs].
* The board draws a picture every 30 ms whether or not anything changed, so the pictures a second (`PICTURES_1S`) read 33 or 34 on a still desktop; the panel's frames a second, as a game counts them, are `CHANGED_1S` [verified: section 5].
* Each value carries a valid bit, so a program can tell "this build does not have it" from 0 (the PC harness has no PSRAM figure, for example).
* Found from DOS (FreeDOS), from a DOS box under Windows 95, with every width of `in` and `out`, and from a Win32 program under Windows 95 (T386MON) [verified: section 9]. A Win16 program should reach it the same way [inferred: section 7].
* It builds for both boards (the ESP32-S31-Korvo-1 and the DOS board) with no new warning [verified: section 9]. It has not yet been read on a board [unknown].


## 2. The ports

| Port | Width | Way | What |
|-|-|-|-|
| 0F1F8h | 16 | write | INDEX: which value DATA gives |
| 0F1F8h | 8 | write | INDEX = that byte (the high byte 0) |
| 0F1F8h | 32 | write | INDEX = the low 16 bits, then COMMAND = the high 16 bits (0 does nothing) |
| 0F1F8h | 16 | read | INDEX as last written |
| 0F1FAh | 16 or 8 | write | COMMAND: 1 takes a snapshot; anything else does nothing |
| 0F1FAh | 16 | read | snapshots taken so far, the low 16 bits |
| 0F1FCh | 32 | read | DATA: the value at INDEX |
| 0F1FCh, 0F1FEh | 16 | read | DATA's low and high halves |
| 0F1FCh to 0F1FFh | 8 | read | DATA's four bytes |

A write to DATA, or at 0F1F9h, 0F1FBh, does nothing. A read that runs past 0F1FFh gives 0FFh for each byte beyond [verified: host test]. A PC without the port gives 0FFFFFFFFh for DATA (tiny386 without the port: `pc.c`'s default) or whatever its bus floats to, never the magic.

`rep insd` on DATA works (each element is one read, as `i386.c` does a string read that `pc.c` has no fast way for) but gives the same value each time: the index does not move by itself [inferred: the code].


## 3. Finding it and reading it

1. `out 0F1F8h, 0` (16 bits); `in 0F1FCh` (32 bits, or two of 16): the value must be `PP_MAGIC`, 36383354h, the four bytes "T386".
2. Index 1, `PP_I_VERSION`: the high 16 bits must be `PP_VERSION_MAJOR` (1). A later minor version only adds indexes (1.1 added 2Eh and 2Fh); a program that wants them checks their valid bits, so it works with 1.0 too.
3. Index 2, `PP_I_COUNT`: how many indexes this build knows (80h in 1.0 and 1.1). Anything at or past it reads 0.
4. Each time round (once a second is plenty): `out 0F1FAh, 1` (16 bits), then for each value `out 0F1F8h, index; in 0F1FCh`.

T386MON (`src/t386mon.c`, `pp_detect()` and `pp_get()`) does this from a Win32 program with its own `in` and `out`. In C with Open Watcom, 16-bit (DOS, Win16) or 32-bit (Win32), `conio.h`'s `outpw()` and `inpw()` should be enough; `int` may be 16 bits, so the values are `unsigned long`. This sketch has not been compiled [inferred]; PPTEST.COM (`port-test/`) does the same in assembly and was run [verified]:

```c
#include <conio.h>
#include "perfport.h"

static unsigned long pp_get(unsigned index)
{
    unsigned long lo, hi;
    outpw(PP_PORT_INDEX, index);
    lo = inpw(PP_PORT_DATA);
    hi = inpw(PP_PORT_DATA + 2);
    return (hi << 16) | lo;
}

static int pp_found(void)
{
    return pp_get(PP_I_MAGIC) == PP_MAGIC &&
           (pp_get(PP_I_VERSION) >> 16) == PP_VERSION_MAJOR;
}

static void pp_snapshot(void)
{
    outpw(PP_PORT_CMD, PP_CMD_SNAPSHOT);
}
```

A value is there if its bit in `PP_I_VALID0` to `PP_I_VALID3` is set (bit n of VALIDk is index 32k + n):

```c
static int pp_valid(unsigned index)
{
    return (int) ((pp_get(PP_I_VALID0 + index / 32) >> (index % 32)) & 1);
}
```

Text (`PP_I_FIRMWARE`, `PP_I_BOARD`): eight indexes of four characters each, the first character in the low byte, padded with 0; at most 31 characters, so there is always a 0.

`perfport.h` is plain C89 with no other header [verified: `gcc -std=c89 -pedantic -Werror` in the host tests]. Every constant that needs more than 16 bits has `UL`. The emulator's own part is at the end, behind `TINY386_PERFPORT`, which a guest program does not define.


## 4. The kinds of value

| Kind | What | How to use it |
|-|-|-|
| live | `PP_I_MAGIC`, `VERSION`, `COUNT`, `SNAPSHOTS`: read at the moment, no snapshot needed | detection |
| total | counts from the start, wrapping at 2^32 | take two snapshots, subtract modulo 2^32 (`unsigned long` arithmetic does it), divide by the difference of `PP_I_CLOCK_US`, also modulo 2^32 |
| last whole second | the emulator's own one-second figures, as the board's Ctrl+Alt+F12 screen shows them | show as they are |
| level | now: a mode, a size, memory free, a longest | show as they are |
| text | two strings | section 3 |

`PP_I_CLOCK_US` is the emulator's microsecond clock and wraps every 71.6 minutes, so a monitor that samples every second or so can always take differences. Every value but the live four is the last snapshot's, and 0 before the first.


## 5. The values

"Board" is the ESP32 firmware (Korvo-1 and DOS board alike); "harness" is the PC harness. Y: filled; 0: valid but always 0 there; -: not filled (valid bit clear, reads 0).

| Index | Name (`PP_I_`) | Unit, kind | Board | Harness | What |
|-|-|-|-|-|-|
| 00h | MAGIC | live | Y | Y | 36383354h |
| 01h | VERSION | live | Y | Y | 00010001h (1.0: 00010000h) |
| 02h | COUNT | live | Y | Y | 80h |
| 03h | SNAPSHOTS | live | Y | Y | snapshots taken since the emulator started (anyone's) |
| 04h to 07h | VALID0 to 3 | bits | Y | Y | which indexes this build fills |
| 08h | SOURCE | | 1 | 2 | 1 the board's firmware, 2 the PC harness |
| 09h | CLOCK_US | us, total | Y | Y | the emulator's clock at the snapshot (the harness: virtual time) |
| 0Ah | UPTIME_MS | ms, total | Y | Y | since the board started (the harness: since the run started) |
| 0Bh | PC_UPTIME_MS | ms, total | Y | Y | since the emulated PC started |
| 0Ch | SNAP_US | us, level | Y | 0 | how long this snapshot took the emulator (the port's own cost) |
| 0Dh | PORT_IO | total | Y | Y | ins and outs of these ports, so the monitor can see its own load |
| 10h, 11h | INSNS_LO, HI | total, 64 bits | Y | Y | guest instructions since the PC started |
| 12h | KIPS | last second | Y | Y | thousand instructions a second of running: halted time and the card's flushes left out (the speed screen's figure) |
| 13h | HALT_PCT | %, last second | Y | Y | of the second, the guest halted (waiting for an interrupt) |
| 14h | RUN_MS | ms, total | Y | Y | time the guest ran |
| 15h | HALT_MS | ms, total | Y | Y | time the guest was halted |
| 16h | SECONDS | total | Y | Y | whole seconds counted for KIPS and HALT_PCT |
| 20h | PICTURES | total | Y | Y | pictures drawn (on the board: core 0, `vga_task`), one every `FRAME_MS` whether or not anything changed (`VGA_RETRACE_REALTIME` makes `vga_step()` say draw every time) |
| 21h | PICTURES_1S | last second | Y | Y | pictures drawn in the last whole second (33 or 34); 0 once no picture has ended for a second (the speed screen up) or two seconds after the last whole second; a second broken by a gap of more than 250 ms between pictures is not counted (1.1) |
| 22h | DRAW_US | us, total | Y | 0 | time spent drawing them, the write-backs to the panel included |
| 23h | DRAW_AVG_US_1S | us, last second | Y | 0 | the average picture |
| 24h | DRAW_MAX_US_1S | us, last second | Y | 0 | the longest picture |
| 25h | DRAW_MAX_US | us, level | Y | 0 | the longest picture since the start |
| 26h | FLUSHES | total | Y | Y | bands of changed rows sent to the screen; on the board each is one write-back from the cache to the panel's frame buffer |
| 27h | FLUSH_ROWS | rows, total | Y | Y | rows in them |
| 28h | VBE_PICTURES | total | Y | Y | VBE pictures drawn from the 1 KiB blocks of video memory the guest wrote |
| 29h | VBE_WHOLE | total | Y | Y | of them, drawn whole (a mode set, a display start or palette change, a clear, the speed screen leaving) |
| 2Ah | VBE_BLOCKS | total | Y | Y | blocks found written |
| 2Bh | VBE_ROWS | rows, total | Y | Y | rows converted from them |
| 2Ch | VBE_BLOCKS_ON | level | Y | Y | `[esp] vbe_blocks`: 1 on, 0 off (every VBE picture drawn whole) |
| 2Dh | FRAME_MS | ms | Y | Y | a picture at most every this many ms (30) |
| 2Eh | CHANGED | total | Y | Y | 1.1: pictures in which at least one row went to the screen (a `redraw()` of one row or more) |
| 2Fh | CHANGED_1S | last second | Y | Y | 1.1: those pictures in the last whole second, the same second as `PICTURES_1S`: the panel's frames a second |
| 30h | MODE_KIND | level | Y | Y | 0 blank, 1 text, 2 VGA graphics, 3 VBE |
| 31h | MODE_WIDTH | pixels | Y | Y | as the guest set it (text: characters); mode 13h is 320 |
| 32h | MODE_HEIGHT | pixels | Y | Y | (text: rows) |
| 33h | MODE_BPP | bits | Y | Y | 2, 4, 8, 15, 16, 24, 32; 0 in text |
| 34h | MODE_LINE_BYTES | bytes | Y | Y | VBE only: a line in video memory |
| 35h | MODE_START | bytes | Y | Y | VBE only: the display start (a page flip moves it) |
| 36h | BIOS_MODE | | Y | Y | the BIOS's mode byte, 0040:0049 (12h: Windows' 16 colours; 03h: text) |
| 37h, 38h | SCREEN_WIDTH, HEIGHT | pixels | Y | Y | the panel (800x480) or the DOS board's picture (800x600) |
| 40h | GUEST_MEM_KB | KiB | Y | Y | `mem_size` |
| 41h | VGA_MEM_KB | KiB | Y | Y | `vga_mem_size` |
| 42h | PSRAM_FREE_KB | KiB | Y | - | PSRAM free outside the emulator's pool |
| 43h | PSRAM_LARGEST_KB | KiB | Y | - | its largest free block |
| 44h | IRAM_FREE_KB | KiB | Y | - | internal RAM free |
| 45h | IRAM_LARGEST_KB | KiB | Y | - | its largest free block |
| 46h | IRAM_MIN_FREE_KB | KiB | Y | - | the least internal RAM free since the board started |
| 50h | DISK_READS | total | Y | Y | reads of the hard disk and CD images |
| 51h | DISK_WRITES | total | Y | Y | writes |
| 52h | DISK_READ_SECTORS | sectors, total | Y | Y | 512 bytes each |
| 53h | DISK_WRITE_SECTORS | sectors, total | Y | Y | |
| 54h | DISK_BUSY_US | us, total | Y | Y | time the emulator waited for those reads and writes |
| 55h | DISK_LONGEST_US | us, level | Y | card | the longest one since the start |
| 56h | DISK_OVER_TICK | total | Y | card | waits longer than a timer tick (55 ms) |
| 57h | DISK_ERRORS | total | Y | Y | accesses the card refused at first |
| 58h | DISK_RECOVERED | total | Y | card | of those, done after the image was opened again |
| 59h | DISK_REFUSED | total | Y | Y | errors the guest was told of (disks and floppies) |
| 5Ah | DISK_FLUSHES | total | Y | Y | flushes to the card after 2 s without a write |
| 5Bh | DISK_ACTIVITY | total | Y | Y | reads and writes of every disk, CD and floppy (the LED's disk light) |
| 60h | TIMER_OWED_MAX | ticks, level | Y | Y | the most timer ticks owed to the guest at once |
| 61h | TIMER_DROPPED | ticks, total | Y | Y | ticks dropped after the emulator was held up |
| 62h | TIMER_CATCHUP | level | Y | Y | `[esp] timer_catchup` |
| 64h | KEY_EVENTS | total | Y | Y | key presses and releases handed to the PS/2 keyboard |
| 65h | MOUSE_REPORTS | total | Y | Y | mouse reports handed to the PS/2 mouse |
| 66h | MOUSE_PACKETS | total | Y | Y | mouse packets sent to the guest (at its sample rate) |
| 68h | LOG_HELD | total | Y | - | repeated log lines held back |
| 69h | LOG_DROPPED | total | Y | - | lines dropped from the UART's queue |
| 6Ah | STATUS | | Y | - | the status LED: stage, the error shown << 8, the last run's end << 16 (stage 6 is "the PC runs") |
| 6Bh | CPU_MHZ | MHz | Y | - | the board's processor clock |
| 70h to 77h | FIRMWARE | text | Y | Y | the firmware's version ("v4.3" in v4.3; the harness: "pc-harness") |
| 78h to 7Fh | BOARD | text | Y | Y | "ESP32-S31-Korvo-1" or "DOS board" (the harness: "PC harness, esp-korvo1") |

"card" in the harness column: filled only when the harness reaches the disk image through the card's FatFs, as the board does; with a plain image file those three stay clear. The indexes not named (0Eh, 0Fh, 17h to 1Fh, 39h to 3Fh, 47h to 4Fh, 5Ch to 5Fh, 63h, 67h, 6Ch to 6Fh) are free for later minor versions; they read 0 with their valid bit clear [verified: host test].

`CHANGED` against the harness's own count of pictures that sent a row: equal at the end of every Windows and DOS run with the port [verified: 16 runs for 1.1, and 15 more when v4.3 was checked].

In the harness the drawing takes no virtual time, so the four DRAW figures are 0 there [verified: the harness's runs]; on the board they are core 0's time [inferred: from the code; section 11].

On the PC the harness also draws pictures of its own: each picture it saves for its record of a run goes through the VBE drawing on the live VGA state. So there the four VBE counts (`VBE_PICTURES`, `VBE_WHOLE`, `VBE_BLOCKS`, `VBE_ROWS`) include the harness's pictures: with a picture saved every 5 s, one more whole picture every 5 s. `PICTURES` and `CHANGED`, counted in `pc_vga_step()`, do not [verified]. On the board only `pc_vga_step()` draws, so every count is the board's own pictures [verified: the firmware's code at v4.3]. Leave the harness's pictures out when counting VBE pictures on the PC.


## 6. What a monitor can show, and from what

| Figure | From |
|-|-|
| The panel's frames a second ("fps") | `CHANGED_1S` (1.1), or the difference of `CHANGED` over the difference of `CLOCK_US`. Not `PICTURES_1S`: the board draws every 30 ms, changed or not |
| Whether the drawing keeps up | `PICTURES_1S` below 33 means pictures took longer than 30 ms on the drawing core |
| The guest's processor load (the task manager's figure) | 100 less `HALT_PCT`. Windows 95 halts when it has nothing to do: in the harness 67 to 89 per cent halted at an idle desktop, 0 while it starts, works or answers dialogs [verified: section 9]. Under DOS it depends on the program: many never halt |
| How fast the guest runs | `KIPS` / 1000, million instructions a second of running, as the speed screen shows it; and instructions a second of wall time, the difference of `INSNS` over the difference of `CLOCK_US` |
| The board's drawing load (core 0) | the difference of `DRAW_US` over the difference of `CLOCK_US`, as a percentage |
| Time a picture takes | `DRAW_AVG_US_1S` and `DRAW_MAX_US_1S`; or the difference of `DRAW_US` over that of `PICTURES` |
| Rows sent to the panel a second | the difference of `FLUSH_ROWS`; write-backs a second: of `FLUSHES` |
| How the VBE drawing is doing | whole pictures a second (`VBE_WHOLE`: each is a whole new screen to convert, 1.15 million instructions at 800x480 in 16 bits), blocks and rows converted a second |
| The mode | `MODE_KIND`, `MODE_WIDTH` x `MODE_HEIGHT` x `MODE_BPP`, against `SCREEN_WIDTH` x `SCREEN_HEIGHT` (larger is shown shrunk) |
| Disk | reads and writes a second, KiB a second (sectors / 2), the average wait (the difference of `DISK_BUSY_US` over that of reads plus writes), the longest |
| The guest's timer | `TIMER_DROPPED` rising: the emulator was held up longer than a tick (55 ms), and the guest's clock fell behind by as much |
| Memory | guest and video memory, PSRAM and internal RAM free |
| Input | key and mouse events a second, mouse packets a second (what the guest's mouse driver handles) |
| The monitor's own cost | `PORT_IO` a second and `SNAP_US` |

On the board the picture's cost is core 0's and the guest's is core 1's; these values are the first way to see both at once there [inferred].


## 7. Under Windows 95

* **A DOS program in a DOS box** reads the port: PPTEST.COM run from Start, Run in the PC harness under the standard VGA driver and under VMDisp9x at 800x480 in 16 bits, every width of `in` and `out`, every check passed [verified]. A DOS box runs in V86 mode, where every `in` and `out` is checked against the I/O permission map in the task state segment; tiny386 checks that map as a 386 does (`check_ioperm()` in `i386.c`), and Windows has not trapped these ports.
* **A Win32 program** doing `in` and `out` itself reaches the port: T386MON does so about 140 times a second in every harness run with the port, with the harness's own values [verified]. Windows 95 lets a ring 3 program reach any port no VxD has trapped, through the same map (the reason old hardware tools ran on it without drivers); a Win16 program should too [inferred]. Windows NT does not, and T386MON does not look for the port there.
* **Windows and its drivers do not touch these ports.** Five boots in the harness with every access to a port tiny386 does not handle logged (8-bit reads too, which tiny386 normally keeps quiet): Windows 95 with the standard VGA driver, with VMDisp9x at 800x480 in 16 and 32 bits, the DOS board's build at 800x600, and FreeDOS. The ports touched were 0278h, 027Ah, 02F2h to 02F7h, 0378h, 037Ah, 0402h, 0421h, 04A1h, 04D0h, 04D1h, 0510h, 0511h, 06F2h to 06F7h, 0A20h, 0E20h and 0E22h; none at or above 1000h [verified]. (tiny386's own emulink ports are handled, so they are not in that list.) Then in six full Windows runs with the port present and read every 10 virtual seconds by the harness, the guest ran exactly as without it (section 9). Windows' "Add New Hardware" search was not run; it probes the ISA range 0100h to 03FFh, which this port is far from [inferred].
* **Why 0F1F8h**: it is next to tiny386's own guest interface (emulink, 0F1F0h to 0F1F7h, which its BIOS uses for the floppies) and handled nowhere else in `pc.c` [verified: `pc.c`]; no PCI device can be given it (tiny386's PCI devices have no I/O window: SeaBIOS says "PCI: IO: c000 - bfff", an empty range) [verified: the boot logs]; it is far above the ISA range that drivers probe, and is none of the ports QEMU, Bochs or VMware use for their own guest interfaces (0402h, 0505h, 0510h, 5658h, 8900h, 0B000h) [inferred].
* **On a real PC** the probe's write to 0F1F8h may reach an ISA card that decodes only ten address bits, as port 01F8h [inferred]. T386MON's `/noport` leaves the probe out.


## 8. Cost

* An `in` or `out` at these ports is a few dozen of the board's instructions: one case of `pc.c`'s port switch and a small function [inferred: the code].
* A snapshot clears 128 words, reads the counters, and on the board asks ESP-IDF's heap for its free figures: five calls, of which the two for the largest free block (PSRAM and internal RAM) each walk every block, used and free, of every heap of that kind inside `portENTER_CRITICAL_SAFE`, so core 0's allocations wait meanwhile [verified: ESP-IDF's `heap_caps_get_largest_free_block()`, `multi_heap_get_info_impl()`, `MULTI_HEAP_LOCK`]. `PP_I_SNAP_US` says what it took, and T386MON shows it on its "Its own cost" line. In the harness it reads 0 (virtual time). On the board [unknown]; if it is more than a few hundred microseconds, the largest blocks can be read every tenth snapshot.
* Every picture reads the clock twice (`get_uticks()`, `esp_system_get_time()` on the board) and counts its bands and whether it sent a row, on core 0 [inferred: the code].
* Memory: 664 bytes more internal RAM on both boards than the firmware without the port (the snapshot's 128 words and the counts), 16 bytes more in the VGA state; the Korvo-1's app image about 3.2 KB larger [verified: `idf.py size`].
* Nothing the guest can see changes unless it reads the port (section 9).


## 9. How it was checked

On the firmware side, in the firmware's own tests and on the PC harness, 2026-10-23 [verified]:

| Check | Result |
|-|-|
| The firmware's host tests | all passed (39), 4 of them new for the port; the host build's warnings unchanged |
| `test_perfport` | 291 checks: detection before any snapshot; DATA by 32, 16 and 8 bits; INDEX read back; an 8-bit index write; a 32-bit write that is index and command at once; commands 0, 2 and 0FFFFh and writes to DATA doing nothing; reads past 0F1FFh; the snapshot holding while the counters move; valid bits for the emulator's values and none for the front end's until it fills them; text cut at 31 characters; the drawing's totals, its last whole second, a stale second, a pause, a hitch counted and a gap not, the clock's wrap; the 32-bit instruction count made 64 bits across its wrap; a drawing thread against snapshots, 200,000 pictures, no total going back; every `PP_I_` of `perfport.h` in the table, none twice, all below `PP_COUNT` |
| `test_vga_info` (both boards' `vga.c`) | 14 checks each: text 80x25, mode 12h 640x480x4, mode 13h 320x200x8, blank, VBE 800x480x16 with its line and start; the VBE counts for a whole picture, one written line, a still picture and a full update |
| `perfport.h` as a guest's header | `gcc -std=c89 -pedantic -Werror -Wall -Wextra`, nothing else included: clean |
| Board builds (`idf.py build` for the Korvo-1 and the DOS board) | exit 0; no new compiler warning |
| PPTEST.COM under FreeDOS | 562 checks, 0 bad: four rounds, each by other widths of `in` and `out`; identity, valid bits, the harness's values, the text mode, counters that move, the picture count against the virtual clock |
| PPTEST.COM in a Windows 95 DOS box, VMDisp9x at 800x480 in 16 bits | 570 checks, 0 bad; the mode read as VBE 800x480x16, 1600 bytes a line (and under the standard VGA driver, as 640x480x4, in 1.0) |
| Six Windows 95 runs (standard VGA with and without the mouse, VMDisp9x 16 bits with and without, 32 bits, the DOS board at 800x600) with the port read every 10 virtual seconds | each run's end state (virtual time, instruction count, registers) and final picture the same as without the port |
| Mutation run | 17 faults put in one at a time (eight in `perfport.c`, four in `vga.c`, three in `pc.c`, one each in `i8042.c` and `disk_image.c`), and for 1.1 three more: every one caught by a host test or PPTEST's checker |
| T386MON 1.2 in Windows 95, 16 harness runs with the port, and 7 more on v4.3's core | `CHANGED` equal to the harness's own count of pictures that sent a row in every run; four runs repeated on v4.3's core and on the core the port was made on gave the same pictures and the same `C:\T386MON.LOG`, byte for byte |


## 10. The firmware side

The port is a few small pieces, all behind `TINY386_PERFPORT`, so tiny386's other front ends build as before:

| File | Change |
|-|-|
| `perfport.h` (new) | the protocol (this repo's copy is the same file), and behind `TINY386_PERFPORT` the emulator's few calls |
| `perfport.c` (new) | the port, the snapshot, the drawing's counts and its last second |
| `pc.c` | six `case` lines in the port switches, and `pc_vga_step()` timing each picture and drawing it through `perfport_redraw()` |
| `vga.c`, `vga.h` | four counts at the end of the VGA state (VBE pictures, whole ones, blocks, rows: one-line increments in the VBE drawing) and `vga_get_info()` |
| `i8042.c`, `i8042.h` | three counts: key events, mouse reports, mouse packets |
| `esp/main/speed.c` | the board's values, from the speed screen's own counters, registered when the PC starts |
| `esp/main/disk_image.c`, `.h` | sectors read and written and the time waited, beside the existing counts |
| `esp/main/status_led.c`, `.h` | what the status LED shows |
| `esp/main/CMakeLists.txt` | `perfport.c` and `TINY386_PERFPORT` |
| `tests/host` | `test_perfport.c`, `test_vga_info.c`, and the disk test's sector check |

Ctrl+Alt+F11 (v4.2 on) turns `vbe_blocks` on and off while the PC runs; `VBE_BLOCKS_ON` gives it as it is at each snapshot [verified: host test, and T386MON's "VBE drawing" line on the harness].


## 11. What only a board can say [unknown]

* Every figure the harness cannot have: the drawing's time on core 0, PSRAM and internal RAM, the status code, the log's counts, the disk's waits on the card.
* What a snapshot costs on the board (`PP_I_SNAP_US`).
* Whether the speed screen and the port agree to the digit (they read the same counters, so they should [inferred]).
* That a Win32 program under Windows 95 on the board reaches the port as T386MON does in the harness (the same guest code, so it should [inferred]).
* What a flush to the card costs the guest, and how often Windows' own writes cause one.
