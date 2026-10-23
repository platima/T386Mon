#!/usr/bin/env python3
# mkicon.py OUT.ico: T386MON's icon, 32x32 and 16x16 in the 16 VGA colours:
# a meter's bars on black in a grey frame.  Made here, so the build needs no
# picture from anywhere else.
import struct
import sys

# the standard 16 colours, in Windows' order (RGBQUAD is blue, green, red, 0)
PAL = [(0, 0, 0), (128, 0, 0), (0, 128, 0), (128, 128, 0), (0, 0, 128), (128, 0, 128),
       (0, 128, 128), (192, 192, 192), (128, 128, 128), (255, 0, 0), (0, 255, 0),
       (255, 255, 0), (0, 0, 255), (255, 0, 255), (0, 255, 255), (255, 255, 255)]
BLACK, GREY, SILVER, LIME, YELLOW, RED, WHITE = 0, 8, 7, 10, 11, 9, 15


def picture(n):
    px = [[BLACK] * n for _ in range(n)]
    for i in range(n):
        for j in (0, n - 1):
            px[i][j] = px[j][i] = GREY
    # five bars rising left to right, green, then yellow, then red at the top
    bars = 5
    inner = n - 4
    bw = max(1, inner // (bars * 2 - 1) + (1 if n >= 32 else 0))
    gap = max(1, (inner - bars * bw) // (bars - 1))
    x = 2
    for b in range(bars):
        h = (inner * (b + 2)) // (bars + 1)
        for y in range(n - 3, n - 3 - h, -1):
            level = (n - 3 - y) * 3 // inner
            colour = (LIME, YELLOW, RED)[min(level, 2)]
            for k in range(bw):
                if x + k < n - 2:
                    px[y][x + k] = colour
        x += bw + gap
    # a white base line
    for k in range(2, n - 2):
        px[n - 3][k] = WHITE
    return px


def dib(px):
    n = len(px)
    head = struct.pack('<IiiHHIIiiII', 40, n, 2 * n, 1, 4, 0, 0, 0, 0, 16, 0)
    pal = b''.join(struct.pack('<BBBB', b, g, r, 0) for r, g, b in PAL)
    xor = b''
    for y in range(n - 1, -1, -1):          # bottom row first
        row = bytearray()
        for x in range(0, n, 2):
            row.append((px[y][x] << 4) | px[y][x + 1])
        while len(row) % 4:
            row.append(0)
        xor += bytes(row)
    and_row = (n + 31) // 32 * 4             # all 0: every pixel shown
    mask = bytes(and_row * n)
    return head + pal + xor + mask


def main():
    imgs = [dib(picture(32)), dib(picture(16))]
    out = struct.pack('<HHH', 0, 1, len(imgs))
    off = 6 + 16 * len(imgs)
    for size, data in zip((32, 16), imgs):
        out += struct.pack('<BBBBHHII', size, size, 16, 0, 1, 4, len(data), off)
        off += len(data)
    out += b''.join(imgs)
    with open(sys.argv[1], 'wb') as f:
        f.write(out)


main()
