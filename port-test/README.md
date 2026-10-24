# PPTEST 🔌

PPTEST.COM is a small DOS program that checks tiny386's information port from inside the emulated PC. It looks for the port, then four times, about a second apart, takes a snapshot and reads every one of the 128 values, each round with other widths of `in` and `out` (32-bit, two 16-bit halves, four bytes, and 32-bit again after writing to the screen), so every way into the port gets used.

It says what it reads on the BIOS's debug port, 0402h, a line at a time (`@pp ROUND INDEX VALUE` in hex), and ends with `@done`. That suits the PC harness I test tiny386's firmware with, which logs port 0402h. It was made for the harness and hasn't been run on a board. Under Windows 95 it runs in a DOS box, which shows that a program in a V86 virtual machine reaches the port too.

| File | |
|-|-|
| `pptest.S` | the program, GNU `as` in Intel syntax |
| `bios.inc` | the output and check helpers it shares with my tiny386 BIOS tests |
| `ppcheck.py` | checks a run's log: the port's identity, a snapshot each round, values 0 wherever the valid bits say none, counters that move, the 16 and 8-bit rounds agreeing with the 32-bit ones |
| `build.sh` | builds PPTEST.COM |
| `LICENSE` | the BSD 3-clause licence that `pptest.S` and `bios.inc` are offered under; the rest is MIT, as the repo is |

```sh
bash port-test/build.sh            # PPTEST.COM in ./out
python3 port-test/ppcheck.py RUNDIR KIND   # RUNDIR/log.txt; KIND text, vga or vbe
```

On the PC harness, PPTEST.COM passed every check under FreeDOS (562 checks) and in a Windows 95 DOS box at 800x480 in 16 bits (570 checks) with port version 1.1.

The built PPTEST.COM isn't kept in the tree: build it, or take it from a CI run's `checks` artifact.
