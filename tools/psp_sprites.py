#!/usr/bin/env python
"""Prescaled 2D images made offline from the player's own ISO (p4sprite.bin).

At run time port/clut32.h swz_find resamples each big 8-bit TMX image (bustups, persona cards, title layers; at
least 256 a side) to the size the PSP shows it at, 3/4 x 17/28 (640x448 -> 480x272), with Lanczos-2 and a palette
match among 16 nearby texels, spread over frames. Here the same image is made with Lanczos-3 (Pillow, premultiplied
alpha) and matched against the whole palette, and the game loads it instead.

P4SP v1: magic, version, count, data offset (u32); sorted rows of (hash, check, width, height, offset) (u32), the
hashes over the raster's indices (width x height bytes) as u32 words; not the palette: the game fades a bustup in
by scaling its palette's alpha, so the first bind sees a palette between states (the copy draws with the raster's
own palette anyway). Hash = (hash ^ w) * 16777619 from 2166136261,
check = (check * 33) ^ w from 5381. Each payload: u16 pw, u16 ph, then pw x ph indices.
"""
import argparse
import multiprocessing
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from psp_clut import iso_files, ISO, DIST  # noqa: E402

PRESCALE_MIN = 256  # port/clut32.h p4_swz_prescale_min


def prescaled_size(w, h):
    return (w * 3 + 3) // 4, (h * 17 + 27) // 28  # port/clut32.h swz_find


def tmx_images(d):
    """(width, height, index bytes, runtime palette words) of each 8-bit TMX image the runtime prescales."""
    o = 0
    while True:
        o = d.find(b'TMX0', o)
        if o < 0:
            return
        s, o = o - 8, o + 4
        if s < 0 or s + 0x40 > len(d) or d[s] != 2 or d[s + 1] != 0:
            continue
        count, pal_format, fmt = d[s + 0x10], d[s + 0x11], d[s + 0x16]
        w, h = struct.unpack_from('<HH', d, s + 0x12)
        if fmt not in (19, 27) or not count or pal_format != 0 or min(w, h) < PRESCALE_MIN:
            continue
        pixels = s + 0x40 + 1024 * count
        if pixels + w * h > len(d):
            continue
        raw = struct.unpack_from('<256I', d, s + 0x40)  # the first palette: the one tmxUploadStream copies
        pal = []
        for i in range(256):  # rwsky.c func_003fd520
            j = i + 8 if i & 24 == 8 else i - 8 if i & 24 == 16 else i
            a = raw[j] >> 24
            pal.append(raw[j] & 0xffffff | (0xff if a >= 0x80 else a * 2) << 24)
        idx = d[pixels:pixels + w * h]
        if w > 512 or h > 512:  # rwsky.c p4_tmx_reduce: point-sampled to 512 a side
            rw, rh = min(w, 512), min(h, 512)
            idx = b''.join(bytes(idx[(y * h // rh) * w + x * w // rw] for x in range(rw)) for y in range(rh))
            w, h = rw, rh
        yield w, h, bytes(idx), pal


def key_of(w, h, idx, pal):
    hsh, check = 2166136261, 5381
    for word in struct.unpack(f'<{len(idx) // 4}I', idx):
        hsh = ((hsh ^ word) * 16777619) & 0xffffffff
        check = ((check * 33) ^ word) & 0xffffffff
    return hsh, check, w, h


def distance(c, r, g, b, a):
    # port/clut32.h swz_nearest: colour weighted by the brighter alpha, alpha counted double
    ca = c >> 24
    dr, dg, db, da = (c & 255) - r, (c >> 8 & 255) - g, (c >> 16 & 255) - b, ca - a
    return ((2 * dr * dr + 4 * dg * dg + db * db) * (max(ca, a) + 8) >> 8) + 6 * da * da


def prescale(w, h, idx, pal):
    from PIL import Image
    pw, ph = prescaled_size(w, h)
    rgba = b''.join(struct.pack('<I', c) for c in pal)
    src = Image.frombytes('RGBA', (w, h), b''.join(rgba[i * 4:i * 4 + 4] for i in idx))
    out = src.resize((pw, ph), Image.Resampling.LANCZOS).tobytes()  # Pillow premultiplies RGBA for the filter
    exact = {}
    for i in range(255, -1, -1):
        exact[pal[i]] = i
    memo = {}
    res = bytearray(pw * ph)
    for y in range(ph):
        sy = min(h - 1, (y * 2 + 1) * h // (2 * ph))
        for x in range(pw):
            centre = idx[sy * w + min(w - 1, (x * 2 + 1) * w // (2 * pw))]
            r, g, b, a = out[(y * pw + x) * 4:(y * pw + x) * 4 + 4]
            if a == 0 and pal[centre] >> 24 == 0:
                res[y * pw + x] = centre
                continue
            c = r | g << 8 | b << 16 | a << 24
            if pal[centre] == c:  # flat areas keep their own index
                res[y * pw + x] = centre
                continue
            best = memo.get(c)
            if best is None:
                best = exact.get(c)
                if best is None:
                    best = min(range(256), key=lambda i: distance(pal[i], r, g, b, a))
                memo[c] = best
            res[y * pw + x] = best
    return struct.pack('<HH', pw, ph) + bytes(res)


def job(item):
    w, h, idx, pal = item
    return key_of(w, h, idx, pal), prescale(w, h, idx, pal)


def images(iso):
    seen = set()
    for _, data in iso_files(iso):
        for w, h, idx, pal in tmx_images(data):
            k = hash((w, h, idx, tuple(pal)))
            if k not in seen:
                seen.add(k)
                yield w, h, idx, pal


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--iso', type=Path, default=ISO)
    ap.add_argument('--out', type=Path, default=DIST / 'p4sprite.bin')
    ap.add_argument('--jobs', type=int, default=None)
    ap.add_argument('--limit', type=int, default=0, help='first N images only (testing)')
    args = ap.parse_args()
    t0 = time.time()
    todo = list(images(args.iso))
    if args.limit:
        todo = todo[:args.limit]
    print(f'{len(todo)} images to prescale ({time.time() - t0:.0f} s)', flush=True)
    rows = {}
    # By module name, not __main__: a frozen Prepare P4.exe does not rebuild __main__ in its pool workers
    # (same as psp_strips, b389).
    import psp_sprites
    with multiprocessing.Pool(args.jobs) as pool:
        for n, (key, payload) in enumerate(pool.imap_unordered(psp_sprites.job, todo, chunksize=4), 1):
            rows[key] = payload
            if n % 100 == 0:
                print(f'  {n}/{len(todo)} ({time.time() - t0:.0f} s)', flush=True)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    temp = args.out.with_suffix('.tmp')
    offset = 16 + len(rows) * 20
    with temp.open('wb') as f:
        f.write(struct.pack('<4sIII', b'P4SP', 1, len(rows), offset))
        for key in sorted(rows):
            f.write(struct.pack('<5I', *key, offset))
            offset += len(rows[key])
        for key in sorted(rows):
            f.write(rows[key])
    temp.replace(args.out)
    print(f'{args.out}: {len(rows)} images, {offset} bytes, {time.time() - t0:.0f} s')


if __name__ == '__main__':
    multiprocessing.freeze_support()
    main()
