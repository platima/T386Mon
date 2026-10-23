#!/usr/bin/env python3
# ppcheck.py RUNDIR KIND: PPTEST.COM's lines in RUNDIR/log.txt, checked.
# KIND says what the guest's screen should be: text (FreeDOS, 80x25),
# vga (Windows' standard driver, 640x480 in 16 colours) or vbe (VMDisp9x,
# 800x480 in 16 bits).  Each round must have the port's identity, a
# snapshot more than the last, its values 0 wherever the valid bits say
# none, and moving counters that move; the rounds read with 16 and 8-bit
# ins must agree with those read with 32.
import re, sys

run, kind = sys.argv[1], sys.argv[2]
log = open(run + "/log.txt", errors="replace").read().splitlines()
vals = {}
for line in log:
    m = re.match(r"^@pp ([0-9a-f]{2}) ([0-9a-f]{2}) ([0-9a-f]{8})$", line)
    if m:
        vals.setdefault(int(m[1], 16), {})[int(m[2], 16)] = int(m[3], 16)
checks = bad = 0
def check(ok, what):
    global checks, bad
    checks += 1
    if not ok:
        bad += 1
        print("BAD", what)

def text(v, first):
    b = b"".join(v.get(first + k, 0).to_bytes(4, "little") for k in range(8))
    return b.split(b"\0")[0].decode("latin-1")

check(any(l.startswith("@ok magic") for l in log), "@ok magic")
check(any(l.startswith("@ok readback") for l in log), "@ok readback")
check(sorted(vals) == [1, 2, 3, 4], "four rounds: %s" % sorted(vals))
MAGIC, VERSION, COUNT = 0x36383354, 0x00010001, 0x80     # 1.1: CHANGED, CHANGED_1S
prev = None
for r in sorted(vals):
    v = vals[r]
    check(len(v) == COUNT, "round %d: %d values" % (r, len(v)))
    if len(v) != COUNT:
        prev = None
        continue
    check(v.get(0) == MAGIC and v.get(1) == VERSION and v.get(2) == COUNT,
          "round %d: identity %s" % (r, [hex(v.get(i, -1)) for i in range(3)]))
    valid = v[4] | v[5] << 32 | v[6] << 64 | v[7] << 96
    for i in range(COUNT):
        if not (valid >> i) & 1:
            check(v[i] == 0, "round %d: index %#x not valid but %#x" % (r, i, v[i]))
    must = [0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
            0x20, 0x21, 0x22, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
            0x30, 0x31, 0x32, 0x33, 0x36, 0x37, 0x38, 0x40, 0x41,
            0x50, 0x51, 0x52, 0x53, 0x54, 0x59, 0x5a, 0x5b, 0x60, 0x61, 0x62,
            0x64, 0x65, 0x66] + list(range(0x70, 0x80))
    for i in must:
        check((valid >> i) & 1, "round %d: index %#x should be valid in the harness" % (r, i))
    check(v[0x08] == 2, "round %d: source %d (2, the harness)" % (r, v[0x08]))
    check(text(v, 0x70) == "pc-harness", "round %d: firmware %r" % (r, text(v, 0x70)))
    check(text(v, 0x78).startswith("PC harness, esp-"), "round %d: board %r" % (r, text(v, 0x78)))
    check(v[0x37] == 800 and v[0x38] in (480, 600), "round %d: screen %dx%d" % (r, v[0x37], v[0x38]))
    want = {"text": (1, 80, 25, 0), "vga": (2, 640, 480, 4), "vbe": (3, 800, 480, 16)}[kind]
    got = (v[0x30], v[0x31], v[0x32], v[0x33])
    check(got == want, "round %d: mode %s, %s wanted" % (r, got, want))
    if kind == "vbe":
        check(v[0x34] == 1600, "round %d: VBE line %d" % (r, v[0x34]))
    check(v[0x12] == 2000, "round %d: kips %d (2000 on the virtual clock)" % (r, v[0x12]))
    if kind != "text":
        # Windows: the dialogs' Enters and the Run line were typed
        check(v[0x64] >= 40, "round %d: %d key events" % (r, v[0x64]))
    check(v[0x2d] == 30, "round %d: frame ms %d" % (r, v[0x2d]))
    if prev:
        p = prev
        check(v[3] > p[3], "round %d: snapshots %d after %d" % (r, v[3], p[3]))
        dt = (v[0x09] - p[0x09]) & 0xffffffff
        check(500000 < dt < 3000000, "round %d: %d us since the last" % (r, dt))
        ins = (v[0x10] | v[0x11] << 32) - (p[0x10] | p[0x11] << 32)
        check(ins > 0, "round %d: instructions moved %d" % (r, ins))
        dp = (v[0x20] - p[0x20]) & 0xffffffff
        check(dp >= dt / 30000 - 3 and dp <= dt / 30000 + 3,
              "round %d: %d pictures in %d us (one each 30 ms)" % (r, dp, dt))
        check(30 <= v[0x21] <= 35, "round %d: %d pictures in the last second" % (r, v[0x21]))
        check(v[0x2f] <= v[0x21], "round %d: %d changed of %d in the last second" %
              (r, v[0x2f], v[0x21]))
        check(((v[0x2e] - p[0x2e]) & 0xffffffff) <= dp,
              "round %d: %d changed of %d pictures" % (r, (v[0x2e] - p[0x2e]) & 0xffffffff, dp))
        check(v[0x0d] > p[0x0d], "round %d: port I/O %d after %d" % (r, v[0x0d], p[0x0d]))
        check(v[0x0b] > p[0x0b] and v[0x0a] > p[0x0a], "round %d: uptimes move" % r)
        for i in (0x00, 0x01, 0x02, 0x2c, 0x30, 0x31, 0x32, 0x33, 0x36, 0x40, 0x41):
            check(v[i] == p[i], "round %d: index %#x changed, %#x to %#x" % (r, i, p[i], v[i]))
    prev = v
if 3 in vals and 4 in vals and len(vals[3]) == COUNT and len(vals[4]) == COUNT:
    # the screen written between rounds 3 and 4
    check((vals[4][0x27] - vals[3][0x27]) & 0xffffffff >= 16,
          "rows sent to the screen after the text was written: %d" %
          ((vals[4][0x27] - vals[3][0x27]) & 0xffffffff))
    check((vals[4][0x2e] - vals[3][0x2e]) & 0xffffffff >= 1,
          "pictures that changed after the text was written: %d" %
          ((vals[4][0x2e] - vals[3][0x2e]) & 0xffffffff))
end = [l for l in log if l.startswith("[harness] perfport END")]
check(len(end) == 1, "the harness's own END line")
print("ppcheck %s %s: %d checks, %d bad; rounds %s; the END line: %s" %
      (run.rsplit("/", 1)[-1], kind, checks, bad, sorted(vals), end[0][:160] if end else "-"))
sys.exit(1 if bad else 0)
