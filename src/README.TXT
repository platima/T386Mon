T386MON 1.2: how fast Windows 95 runs on tiny386
=================================================

Start, Run, B:\T386MON.EXE (this floppy in B:, with another floppy such
as vbe-reg.img in A:, as for the display driver; with nothing in A:,
Windows cannot read B:). Or copy T386MON.EXE to C:\ once, make a
shortcut, and take the floppies out of the ini. It installs nothing and
changes nothing in Windows.

The window shows, once a second:
  - at the top: the panel's frames a second (fps: pictures on the panel
    that changed; the board draws one every 30 ms whether or not
    anything changed; a still desktop shows 1 to 3 fps, T386MON's own
    figures among them), how busy the guest processor is, how fast it
    runs (million instructions a second), and the rows sent to the
    panel a second;
  - Windows' figures: memory, the swap file and disk cache, paging, the
    file system, the display mode;
  - tiny386's figures, from its information port: the pictures and what
    each took the board to draw, the drawing, the mode, memory, the disk,
    the timer, the keyboard and mouse. Without the port (older firmware,
    or another PC) it says it is not running on tiny386.

Buttons and keys:
  Live (F2)           the figures above
  Tests (F3)          the drawing tests' results; maximise the window for
                      the largest text, to photograph
  Run all tests (F5)  idle, fill, scroll, text (with the mouse pointer on
                      the drawing, then beside it, then hidden), sprite,
                      full screen copy and full screen picture, 4 s each;
                      about 80 s on the PC harness. The pointer is parked
                      for the tests and put back after: leave the mouse
                      alone. Esc stops them.
  Log now (F6)        writes the whole live view to the log
  Compact (F4)        a small window on top with the main figures;
                      double-click it (or F4) for the full view
  On top              keeps the window above the others
  Windows CPU (F7)    Windows' own processor figure. Off on tiny386:
                      while it is on, Windows never halts, so the
                      emulator sees the guest 100% busy.

What it costs the guest at 2 million instructions a second (PC harness):
about 6% in the live view at 800x480 in 16 bits (4% with the standard
VGA driver), less compact, about 1% on the tests view or minimised.

The log is C:\T386MON.LOG: a line every 5 seconds, every test's results
and a summary. The lines are written to the disk every 60 seconds, at
Log now, after the tests and when T386MON closes or Windows shuts down
(each write costs the board a flush to the card), never during a test.
Close T386MON or shut Windows down before copying the disk image off
the card, so that the last lines are written.

Switches: /bench (the tests 3 s after the start), /quit (close after
them), /secs:N (seconds a test), /every:N (a log line every N seconds;
0 none), /write:N (to the disk every N seconds; 0 at once), /nolog,
/log:PATH, /refresh:N (the live view every N seconds), /compact, /top,
/wcpu or /nowcpu, /? (this list).

The package's README.md says what every figure and log column means.
