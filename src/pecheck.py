#!/usr/bin/env python3
# pecheck.py EXE ALLOWED [W95REF]: is EXE a Win32 program Windows 95 can load?
#
#   - an i386 PE, GUI, subsystem and OS version 4.0, image base 400000h,
#     file alignment 200h and section alignment 1000h;
#   - no TLS, load configuration, bound or delayed imports, no .NET header;
#   - the import table in a writable section;
#   - imports by name only, from KERNEL32, USER32, GDI32 and ADVAPI32 only,
#     and each one in ALLOWED (lines "DLL Name"), the list checked against
#     Windows 95 OSR2's own DLLs;
#   - with W95REF, a folder of DLL.exports files (the export names of Windows
#     95's own DLLs, one a line), each import also in its DLL's list there.
# Prints what it found; exit 1 on any problem.
import os
import struct
import sys

problems = []


def bad(msg):
    problems.append(msg)
    print('PROBLEM: ' + msg)


def main():
    argv = sys.argv[1:]
    code = None
    if len(argv) > 1 and argv[0] == '--code':
        code = int(argv[1], 16)
        argv = argv[2:]
    exe, allowed_path = argv[0], argv[1]
    ref = argv[2] if len(argv) > 2 else None
    d = open(exe, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    if d[pe:pe + 4] != b'PE\0\0':
        bad('no PE signature')
        return
    machine, nsec, _, _, _, optsize, chars = struct.unpack_from('<HHIIIHH', d, pe + 4)
    opt = pe + 24
    magic = struct.unpack_from('<H', d, opt)[0]
    print('machine %04x, %d sections, characteristics %04x, optional header %04x' %
          (machine, nsec, chars, magic))
    if machine != 0x14c or magic != 0x10b:
        bad('not an i386 PE32')
    (entry, base, salign, falign, osmaj, osmin, imgmaj, imgmin, ssmaj, ssmin) = \
        struct.unpack_from('<I8xI II HHHHHH', d, opt + 16)
    subsystem, dllchars = struct.unpack_from('<HH', d, opt + 68)
    nrva = struct.unpack_from('<I', d, opt + 92)[0]
    dirs = [struct.unpack_from('<II', d, opt + 96 + 8 * i) for i in range(nrva)]
    print('entry %08x, base %08x, alignment %x/%x, OS %d.%d, subsystem %d version %d.%d, '
          'DLL characteristics %04x' % (entry, base, salign, falign, osmaj, osmin, subsystem,
                                         ssmaj, ssmin, dllchars))
    if base != 0x400000:
        bad('image base %x' % base)
    if salign != 0x1000 or falign != 0x200:
        bad('alignment %x/%x' % (salign, falign))
    if (osmaj, osmin) != (4, 0) or (ssmaj, ssmin) != (4, 0):
        bad('OS %d.%d, subsystem %d.%d: Windows 95 wants 4.0' % (osmaj, osmin, ssmaj, ssmin))
    if subsystem != 2:
        bad('subsystem %d, not GUI' % subsystem)
    names = ['export', 'import', 'resource', 'exception', 'security', 'reloc', 'debug',
             'arch', 'globalptr', 'TLS', 'load config', 'bound import', 'IAT', 'delay import',
             'CLR', 'reserved']
    for i, (rva, size) in enumerate(dirs):
        if size:
            print('  directory %-12s %08x %6d' % (names[i], rva, size))
    for i in (9, 10, 11, 13, 14):
        if i < nrva and dirs[i][1]:
            bad('has a %s directory' % names[i])
    secs = []
    sh = opt + optsize
    for i in range(nsec):
        name, vsize, va, rsize, rptr, _, _, _, _, sch = struct.unpack_from('<8sIIIIIIHHI', d,
                                                                            sh + 40 * i)
        name = name.rstrip(b'\0').decode()
        secs.append((name, va, vsize, rptr, rsize, sch))
        print('  section %-8s va %08x size %6x file %6x+%6x flags %08x' %
              (name, va, vsize, rptr, rsize, sch))

    if code is not None:
        # the program's code is t386mon.o's .text, which cpucheck.py looks at;
        # the link adds only ld's two empty constructor and destructor lists
        # (-1, 0 each), data that is never run
        text = [s for s in secs if s[0] == '.text']
        tail = bytes.fromhex('ffffffff00000000ffffffff00000000')
        if len(text) != 1:
            bad('not one .text section')
        else:
            name, va, vsize, rptr, rsize, sch = text[0]
            got = d[rptr + code:rptr + code + 16]
            print('.text: %x bytes, the object\'s code %x and %s' %
                  (vsize, code, 'the two empty lists' if got == tail and vsize == code + 16
                   else 'something else'))
            if vsize != code + 16 or got != tail:
                bad('.text holds more than the object\'s code and the two empty lists')

    def off(rva):
        for name, va, vsize, rptr, rsize, sch in secs:
            if va <= rva < va + max(vsize, rsize):
                return rptr + rva - va, sch
        raise ValueError('rva %x in no section' % rva)

    def cstr(o):
        return d[o:d.index(b'\0', o)].decode()

    allowed = set()
    for line in open(allowed_path):
        line = line.split('#')[0].strip()
        if line:
            dll, fn = line.split()
            allowed.add((dll.upper(), fn))
    irva = dirs[1][0]
    io, isch = off(irva)
    if not isch & 0x80000000:
        bad('the import table is in a section that is not writable')
    imports = []
    while True:
        oft, _, _, namerva, ft = struct.unpack_from('<IIIII', d, io)
        if not namerva:
            break
        dll = cstr(off(namerva)[0]).upper()
        if dll not in ('KERNEL32.DLL', 'USER32.DLL', 'GDI32.DLL', 'ADVAPI32.DLL'):
            bad('imports from %s' % dll)
        if not isch & 0x80000000 or not off(ft)[1] & 0x80000000:
            bad('%s: its address table is not writable' % dll)
        t = off(oft or ft)[0]
        while True:
            e = struct.unpack_from('<I', d, t)[0]
            if not e:
                break
            if e & 0x80000000:
                bad('%s: an import by ordinal %d' % (dll, e & 0xffff))
            else:
                imports.append((dll, cstr(off(e)[0] + 2)))
            t += 4
        io += 20
    print('%d imports:' % len(imports))
    exports = {}
    for dll, fn in imports:
        mark = ''
        if (dll, fn) not in allowed:
            bad('%s %s is not in the list of functions checked against Windows 95' % (dll, fn))
            mark += ' NOT-LISTED'
        if ref:
            key = dll.split('.')[0]
            if key not in exports:
                p = os.path.join(ref, key + '.exports')
                exports[key] = set(open(p).read().split()) if os.path.exists(p) else None
            if exports[key] is None:
                bad('no export list for %s in %s' % (key, ref))
            elif fn not in exports[key]:
                bad('%s %s is not exported by Windows 95' % (dll, fn))
                mark += ' NOT-IN-WIN95'
            else:
                mark += ' (in Windows 95)'
        if fn.endswith('W') and not fn.endswith('Window') and fn not in ('ShowWindow',):
            # Windows 95 has most ...W functions only as stubs that fail
            bad('%s %s looks like a Unicode function' % (dll, fn))
        print('  %-13s %s%s' % (dll, fn, mark))
    print('pecheck: %s' % ('%d problems' % len(problems) if problems else 'OK'))
    sys.exit(1 if problems else 0)


main()
