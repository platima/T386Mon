#!/usr/bin/env python3
# cpucheck.py DISASSEMBLY: is every instruction one a 386 with no FPU runs?
# DISASSEMBLY is objdump -d's output for the program.  Flags x87 instructions
# (tiny386 runs Windows 95 with fpu = 0), MMX and SSE registers, and the
# instructions the 486 and later added (bswap, cmpxchg, xadd, invd, wbinvd,
# invlpg, cpuid, rdtsc, cmov, the long nop 0F 1F, ...).
# Prints a count of each mnemonic; exit 1 on any problem.
import collections
import re
import sys

LATER = {'bswap', 'cmpxchg', 'cmpxchg8b', 'xadd', 'invd', 'wbinvd', 'invlpg', 'cpuid',
         'rdtsc', 'rdtscp', 'rdmsr', 'wrmsr', 'rdpmc', 'rsm', 'sysenter', 'sysexit', 'syscall',
         'sysret', 'ud2', 'nopl', 'nopw', 'emms', 'femms', 'lfence', 'mfence', 'sfence',
         'clflush', 'popcnt', 'lzcnt', 'tzcnt', 'movbe', 'endbr32', 'prefetch', 'prefetchw',
         'prefetchnta', 'prefetcht0', 'prefetcht1', 'prefetcht2', 'wait', 'fwait'}
count = collections.Counter()
problems = 0
line_re = re.compile(r'^\s*([0-9a-f]+):\t([0-9a-f ]+)\t(\S+)\s*(.*)$')
for line in open(sys.argv[1]):
    m = line_re.match(line)
    if not m:
        continue
    addr, raw, mn, ops = m.groups()
    # a prefix shown on its own (rep, lock, data16) with the instruction after it
    parts = (mn + ' ' + ops).split()
    while parts and parts[0] in ('rep', 'repz', 'repnz', 'repe', 'repne', 'lock', 'data16',
                                 'addr16', 'cs', 'ds', 'es', 'ss', 'fs', 'gs'):
        parts.pop(0)
    if not parts:
        count['(prefix)'] += 1
        continue
    mn = parts[0]
    ops = ' '.join(parts[1:])
    count[mn] += 1
    why = None
    if mn.startswith('f'):
        why = 'x87'
    elif mn in LATER or mn.startswith('cmov') or mn.startswith('fcmov'):
        why = 'not on a 386'
    elif re.search(r'%x?mm\d|%st\b|%st\(', ops):
        why = 'MMX, SSE or x87 register'
    elif raw.replace(' ', '').startswith('0f1f'):
        why = 'long nop (0F 1F), not on a 386'
    if why:
        problems += 1
        print('PROBLEM %s: %s' % (why, line.rstrip()))
print('%d instructions, %d kinds: %s' % (sum(count.values()), len(count),
                                         ' '.join('%s %d' % kv for kv in sorted(count.items()))))
print('cpucheck: %s' % ('%d problems' % problems if problems else 'OK: 386 integer only'))
sys.exit(1 if problems else 0)
