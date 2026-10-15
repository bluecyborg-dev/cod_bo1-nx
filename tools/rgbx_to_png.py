#!/usr/bin/env python3
"""Converts the native renderer's debug dumps to PNG: .img resolve dumps (16-byte header: width, height, VkFormat,
bytes per pixel; float formats are scaled so their largest value is white, and the value range is printed), and the
frame captures (--bo1_debug_capture_every/_dir: .rgbx files with an 8-byte
width/height header, then R8G8B8X8 rows) to PNG, with only the standard library.

Usage: python3 tools/rgbx_to_png.py <file.rgbx | folder> [--scale N]   (writes <name>.png next to each input;
--scale 2 halves the size, for a quick look)."""
import os
import struct
import sys
import zlib


def convert(path, scale):
    with open(path, 'rb') as f:
        w, h = struct.unpack('<II', f.read(8))
        data = f.read()
    ow, oh = w // scale, h // scale
    rows = []
    for y in range(oh):
        row = data[(y * scale) * w * 4:(y * scale) * w * 4 + w * 4]
        rgb = bytearray()
        for x in range(ow):
            o = x * scale * 4
            rgb += row[o:o + 3]
        rows.append(b'\0' + bytes(rgb))
    raw = zlib.compress(b''.join(rows), 6)

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xFFFFFFFF)

    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', ow, oh, 8, 2, 0, 0, 0)) + \
        chunk(b'IDAT', raw) + chunk(b'IEND', b'')
    out = os.path.splitext(path)[0] + '.png'
    with open(out, 'wb') as f:
        f.write(png)
    return out


def unpack_pixel(fmt, px):
    """RGB floats of one pixel of a dump."""
    if fmt in (37, 44):
        r, g, b = px[0] / 255, px[1] / 255, px[2] / 255
        return (b, g, r) if fmt == 44 else (r, g, b)
    if fmt == 4:  # R5G6B5_UNORM_PACK16
        v = struct.unpack('<H', px)[0]
        return ((v >> 11) / 31, ((v >> 5) & 63) / 63, (v & 31) / 31)
    if fmt == 16:  # R8G8_UNORM
        return (px[0] / 255, px[1] / 255, 0.0)
    if fmt == 64:
        v = struct.unpack('<I', px)[0]
        return ((v & 1023) / 1023, ((v >> 10) & 1023) / 1023, ((v >> 20) & 1023) / 1023)
    if fmt == 100:
        d = struct.unpack('<f', px)[0]
        return (d, d, d)
    if fmt == 97:
        h = struct.unpack('<4e', px)
        return h[:3]
    if fmt == 91:
        v = struct.unpack('<4H', px)
        return tuple(c / 65535 for c in v[:3])
    if fmt == 92:
        v = struct.unpack('<4h', px)
        return tuple(max(c / 32767, -1) for c in v[:3])
    if fmt == 83:
        h = struct.unpack('<2e', px)
        return (h[0], h[1], 0.0)
    if fmt == 77:
        v = struct.unpack('<2H', px)
        return (v[0] / 65535, v[1] / 65535, 0.0)
    if fmt == 78:
        v = struct.unpack('<2h', px)
        return (v[0] / 32767, v[1] / 32767, 0.0)
    if fmt == 103:
        v = struct.unpack('<2f', px)
        return (v[0], v[1], 0.0)
    raise ValueError(f'format {fmt}')


def write_png(out, ow, oh, rows):
    raw = zlib.compress(b''.join(b'\0' + bytes(r) for r in rows), 6)

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xFFFFFFFF)

    with open(out, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', ow, oh, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', raw) + chunk(b'IEND', b''))


def rgb565(v):
    return ((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)


def bc_color_block(b):
    c0, c1, bits = struct.unpack('<HHI', b[:8])
    p0, p1 = rgb565(c0), rgb565(c1)
    if c0 > c1:
        pal = [p0, p1, tuple((2 * a + c) // 3 for a, c in zip(p0, p1)), tuple((a + 2 * c) // 3 for a, c in zip(p0, p1))]
    else:
        pal = [p0, p1, tuple((a + c) // 2 for a, c in zip(p0, p1)), (0, 0, 0)]
    return [pal[(bits >> (2 * i)) & 3] for i in range(16)]


def bc_alpha_block(b, signed=False):
    a0, a1 = (struct.unpack('<bb', b[:2]) if signed else (b[0], b[1]))
    bits = int.from_bytes(b[2:8], 'little')
    if a0 > a1:
        pal = [a0, a1] + [((6 - i) * a0 + (1 + i) * a1) // 7 for i in range(6)]
    else:
        pal = [a0, a1] + [((4 - i) * a0 + (1 + i) * a1) // 5 for i in range(4)] + [(-128 if signed else 0), (127 if signed else 255)]
    vals = [pal[(bits >> (3 * i)) & 7] for i in range(16)]
    return [(v + 128) * 255 // 255 if signed else v for v in vals]


def decode_bc(fmt, data, w, h):
    """BC1 (131/133), BC2 (135), BC3 (137), BC4 (139), BC5 (141/142) to RGB rows."""
    bw, bh = (w + 3) // 4, (h + 3) // 4
    size = 8 if fmt in (131, 133, 139) else 16
    rows = [bytearray(w * 3) for _ in range(h)]
    for by in range(bh):
        for bx in range(bw):
            b = data[(by * bw + bx) * size:(by * bw + bx + 1) * size]
            if len(b) < size:
                continue
            if fmt in (131, 133):
                px = bc_color_block(b)
            elif fmt in (135, 137):
                px = bc_color_block(b[8:])
            elif fmt == 139:
                px = [(a, a, a) for a in bc_alpha_block(b)]
            else:
                r = bc_alpha_block(b[:8], fmt == 142)
                g = bc_alpha_block(b[8:], fmt == 142)
                px = [(rr, gg, 128) for rr, gg in zip(r, g)]
            for i, c in enumerate(px):
                x, y = bx * 4 + i % 4, by * 4 + i // 4
                if x < w and y < h:
                    rows[y][x * 3:x * 3 + 3] = bytes(max(0, min(255, v)) for v in c)
    return rows


def convert_img(path, scale):
    with open(path, 'rb') as f:
        w, h, fmt, bpp = struct.unpack('<4I', f.read(16))
        data = f.read()
    if 131 <= fmt <= 142:
        rows = decode_bc(fmt, data, w, h)
        out = os.path.splitext(path)[0] + '.png'
        write_png(out, w // scale, h // scale,
                  [r[::1] if scale == 1 else bytearray(b''.join(bytes(r[x * scale * 3:x * scale * 3 + 3]) for x in range(w // scale))) for r in rows[::scale]])
        print(f'{out}: {w}x{h} block format {fmt}')
        return
    ow, oh = w // scale, h // scale
    pixels = []
    lo, hi = float('inf'), float('-inf')
    for y in range(oh):
        row = []
        for x in range(ow):
            o = ((y * scale) * w + x * scale) * bpp
            rgb = unpack_pixel(fmt, data[o:o + bpp])
            rgb = tuple(0.0 if c != c else c for c in rgb)
            lo = min(lo, *rgb)
            hi = max(hi, *rgb)
            row.append(rgb)
        pixels.append(row)
    norm = hi if fmt in (97, 100, 83, 103) and hi > 1.0 else 1.0
    rows = [bytearray(int(max(0.0, min(1.0, c / norm)) * 255) for px in row for c in px) for row in pixels]
    out = os.path.splitext(path)[0] + '.png'
    write_png(out, ow, oh, rows)
    print(f'{out}: {w}x{h} format {fmt}, values {lo:.4g}..{hi:.4g}')


def main():
    args = sys.argv[1:]
    scale = 1
    if '--scale' in args:
        i = args.index('--scale')
        scale = int(args[i + 1])
        del args[i:i + 2]
    target = args[0]
    files = [os.path.join(target, n) for n in sorted(os.listdir(target)) if n.endswith(('.rgbx', '.img'))] \
        if os.path.isdir(target) else [target]
    for p in files:
        if p.endswith('.img'):
            convert_img(p, scale)
        else:
            print(convert(p, scale))


if __name__ == '__main__':
    main()
