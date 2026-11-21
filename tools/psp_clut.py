#!/usr/bin/env python
"""Build an optional offline CLUT pack from the player's own ISO.

P4CL v2: magic, version, count, data offset (u32); sorted rows of
(two u32 hashes of normalized RGBA, u32 width, u32 height, u32 offset).
Each payload has u32 depth, RGBA palette, then linear indices (low nibble first).
"""
import argparse
import collections
import os
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from psp_disc import walk, root, ISO, SECTOR, CVM_BASE, DIST  # noqa: E402

SKIP = {'ADX', 'AHX', 'SFD', 'ACX', 'SE', 'AFS'}
PS2 = 0x00325350
GIF_HEADER = 0x50
VERSION_MIN, VERSION_MAX = 0x35000, 0x37002  # port/rwsky.c CHUNK_VERSION_*


def iso_files(iso):
    with open(iso, 'rb') as f:
        lba, size = root(f, 0)
        for path, start, length in list(walk(f, 0, lba, size)):
            if not path.upper().endswith('.CVM'):
                continue
            base = start * SECTOR + CVM_BASE
            try:
                rl, rs = root(f, base)
            except AssertionError:
                continue
            for name, at, n in list(walk(f, base, rl, rs)):
                if name.rsplit('.', 1)[-1].upper() in SKIP:
                    continue
                f.seek(base + at * SECTOR)
                yield path + '/' + name, f.read(n)


def version_ok(stamp):
    v = ((stamp >> 14 & 0x3ff00) + 0x30000) | (stamp >> 16 & 0x3f) if stamp & 0xffff0000 else stamp << 8
    return VERSION_MIN <= v <= VERSION_MAX


def header(d, o):
    return struct.unpack_from('<III', d, o) if o + 12 <= len(d) else (None, 0, 0)


def textures(d):
    """(name, width, height, depth, pixel bytes) of every PS2 native texture chunk found by its header."""
    o = 0
    while True:
        o = d.find(b'\x15\x00\x00\x00', o)
        if o < 0:
            return
        found = None
        try:
            found = parse_native(d, o)
        except (struct.error, IndexError, ValueError):
            found = None
        if found:
            yield found[0]
            o = found[1]
        else:
            o += 1


def parse_native(d, o):
    kind, size, stamp = header(d, o)
    end = o + 12 + size
    if kind != 0x15 or end > len(d) or not version_ok(stamp):
        return None
    p = o + 12
    k, s, st = header(d, p)  # struct: platform, filtering
    if k != 1 or s < 8 or st != stamp or struct.unpack_from('<I', d, p + 12)[0] != PS2:
        return None
    p += 12 + s
    names = []
    for _ in range(2):  # name, mask
        k, s, st = header(d, p)
        if k != 2 or p + 12 + s > end:
            return None
        names.append(d[p + 12:p + 12 + s].split(b'\0', 1)[0].decode('latin-1'))
        p += 12 + s
    k, s, st = header(d, p)  # raster struct
    if k != 1 or p + 12 + s > end:
        return None
    q = p + 12
    k, s, st = header(d, q)  # RasterInfo
    if k != 1 or s < 64:
        return None
    width, height, depth = struct.unpack_from('<iii', d, q + 12)
    fmt, version = struct.unpack_from('<HH', d, q + 24)
    pixel_size, palette_size = struct.unpack_from('<II', d, q + 12 + 44)
    q += 12 + s
    k, s, st = header(d, q)  # data
    if k != 1 or q + 12 + s > end or not (0 < width <= 1024 and 0 < height <= 1024 and depth in (4, 8, 16, 32)):
        return None
    blob = q + 12
    pixels = None
    if version >= 2 and depth >= 16:
        stride = width * depth // 8
        pixels = d[blob + GIF_HEADER:blob + GIF_HEADER + stride * height]
        if GIF_HEADER + stride * height > min(s, pixel_size):
            pixels = None
    return (names[0], width, height, depth, fmt, pixels), end


def fnv1a(*parts):
    h = 2166136261
    for data in parts:
        for b in data:
            h = ((h ^ b) * 16777619) & 0xffffffff
    return h


def key_of(width, height, depth, pixels):
    return fnv1a(struct.pack('<III', width, height, depth), pixels)


ALPHA = bytes(min(255, a * 2) for a in range(256))


def rgba_pixels(pixels):
    rgba = bytearray(pixels)
    rgba[3::4] = pixels[3::4].translate(ALPHA)
    return bytes(rgba)


def palette_data(rgba, width, height):
    colours = list(dict.fromkeys(struct.iter_unpack('<I', rgba)))
    if len(colours) <= 256:
        depth = 4 if len(colours) <= 16 and width % 2 == 0 else 8
        lookup = {c: i for i, c in enumerate(colours)}
        indices = bytes(lookup[c] for c in struct.iter_unpack('<I', rgba))
        palette = b''.join(struct.pack('<I', c[0]) for c in colours).ljust(4 << depth, b'\0')
    else:
        from PIL import Image
        image = Image.frombytes('RGBA', (width, height), rgba).quantize(colors=256, method=Image.Quantize.FASTOCTREE)
        depth, indices = 8, image.tobytes()
        palette = bytes(image.getpalette('RGBA')).ljust(1024, b'\0')
    if depth == 4:
        indices = bytes(a | b << 4 for a, b in zip(indices[::2], indices[1::2]))
    return struct.pack('<I', depth) + palette + indices


def write_pack(args):
    rows = {}
    for _, data in iso_files(args.iso):
        for _, w, h, depth, _, pixels in textures(data):
            if depth != 32 or pixels is None or max(w, h) > 512 or w * h < args.min_texels:
                continue
            rgba = rgba_pixels(pixels)
            check = 5381
            for byte in rgba:
                check = ((check * 33) ^ byte) & 0xffffffff
            key = fnv1a(rgba), check, w, h
            if key not in rows:
                rows[key] = palette_data(rgba, w, h)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    temp = args.out.with_suffix('.tmp')
    with temp.open('wb') as f:
        offset = 16 + len(rows) * 20
        f.write(struct.pack('<4sIII', b'P4CL', 2, len(rows), offset))
        for key in sorted(rows):
            f.write(struct.pack('<5I', *key, offset))
            offset += len(rows[key])
        for key in sorted(rows):
            f.write(rows[key])
    temp.replace(args.out)
    print(f'{args.out}: {len(rows)} textures, {offset} bytes')


def census(args):
    by_depth = collections.Counter()
    bytes_depth = collections.Counter()
    seen = {}
    files = 0
    started = time.time()
    for path, data in iso_files(args.iso):
        files += 1
        for name, w, h, depth, fmt, pixels in textures(data):
            by_depth[depth] += 1
            bytes_depth[depth] += w * h * depth // 8
            if depth >= 16 and pixels is not None:
                k = key_of(w, h, depth, pixels)
                if k not in seen:
                    seen[k] = (w * h, name, w, h, depth, path)
    print(f'{files} files, {time.time() - started:.0f} s')
    for depth in sorted(by_depth):
        print(f'depth {depth:2}: {by_depth[depth]:6} textures (with repeats), {bytes_depth[depth] / 2**20:8.1f} MB of texels')
    unique = sorted(seen.values(), reverse=True)
    big = [u for u in unique if u[0] >= args.min_texels]
    print(f'distinct 16/32-bit textures: {len(unique)}, of them >= {args.min_texels} texels: {len(big)}, '
          f'{sum(u[0] * u[4] // 8 for u in big) / 2**20:.1f} MB -> CLUT8 {sum(u[0] + 1024 for u in big) / 2**20:.1f} MB')
    folders = collections.Counter(u[5].rsplit('/', 1)[0] for u in big)
    for folder, n in folders.most_common(20):
        print(f'  {n:5}  {folder}')
    for u in big[:40]:
        print(f'  {u[2]}x{u[3]}x{u[4]}  {u[1]}  {u[5]}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--iso', type=Path, default=ISO)
    ap.add_argument('--out', type=Path, default=DIST / 'p4clut.bin')
    ap.add_argument('--census', action='store_true', help='numbers only, no file')
    ap.add_argument('--min-texels', type=int, default=32 * 32)
    args = ap.parse_args()
    if args.census:
        census(args)
        return
    write_pack(args)


if __name__ == '__main__':
    main()
