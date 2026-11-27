"""Rough software render of a PPSSPP GE dump (PPSSPPGE v6), to see a frame captured on the PSP without PPSSPP.

  python -I tools/ge_dump_render.py DUMP.ppdmp OUT.png [--draws A-B] [--only N,M] [--owner]

Uses tools/ge_dump_tris.py for the command walk and vertex transform. Per triangle: perspective-correct UV,
nearest texel from CLUT4/CLUT8/16/32-bit textures (swizzled or not), vertex colour or material colour modulate,
alpha test, a w-buffer for the depth test, ordinary alpha blending. No lighting, fog, texture functions other
than modulate, mipmaps or render targets: enough to recognise geometry and textures, not to judge colour.
--owner also writes OUT.owner.png, each pixel coloured by the draw that wrote it last, and prints the draw
numbers under --probe X,Y points.
"""
import argparse
import colorsys
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).parent))
import ge_dump_tris as g  # noqa: E402

W, H = 480, 272
TEXTURE0, CLUT = 0x10, 4


def clut_colours(data, fmt):
    if fmt == 3:
        a = np.frombuffer(data[:len(data) // 4 * 4], np.uint32)
        return np.stack([a & 255, a >> 8 & 255, a >> 16 & 255, a >> 24 & 255], 1).astype(np.uint8)
    a = np.frombuffer(data[:len(data) // 2 * 2], np.uint16).astype(np.uint32)
    if fmt == 0:
        r, gg, b, al = (a & 31) * 255 // 31, (a >> 5 & 63) * 255 // 63, (a >> 11 & 31) * 255 // 31, a * 0 + 255
    elif fmt == 1:
        r, gg, b, al = (a & 31) * 255 // 31, (a >> 5 & 31) * 255 // 31, (a >> 10 & 31) * 255 // 31, (a >> 15) * 255
    else:
        r, gg, b, al = (a & 15) * 17, (a >> 4 & 15) * 17, (a >> 8 & 15) * 17, (a >> 12 & 15) * 17
    return np.stack([r, gg, b, al], 1).astype(np.uint8)


def texture_image(st, tex, clut):
    psm = st.reg[0xC3] & 0xF
    tw, th = 1 << (st.reg[0xB8] & 0xF), 1 << (st.reg[0xB8] >> 8 & 0xF)
    bufw = st.reg[0xA8] & 0xFFFF or tw
    bits = {0: 16, 1: 16, 2: 16, 3: 32, 4: 4, 5: 8, 6: 16, 7: 32}.get(psm)
    if bits is None or not tex:
        return None
    rowbytes = bufw * bits // 8
    raw = np.frombuffer(tex, np.uint8)
    need = rowbytes * th
    if len(raw) < need:
        raw = np.concatenate([raw, np.zeros(need - len(raw), np.uint8)])
    raw = raw[:need]
    if st.reg[0xC2] & 1:  # swizzled: 16-byte x 8-row blocks
        bw = rowbytes // 16
        blocks = raw.reshape(th // 8 if th >= 8 else 1, bw, 8, 16) if th >= 8 else raw.reshape(1, bw, th, 16)
        raw = blocks.transpose(0, 2, 1, 3).reshape(-1)
    rows = raw.reshape(th, rowbytes)
    if psm == 4:
        idx = np.stack([rows & 15, rows >> 4], 2).reshape(th, -1)[:, :tw].astype(np.uint32)
    elif psm == 5:
        idx = rows[:, :tw].astype(np.uint32)
    elif psm in (6, 7):
        width = 2 if psm == 6 else 4
        idx = rows.view('<u2' if width == 2 else '<u4')[:, :tw].astype(np.uint32)
    else:
        fmt = psm
        px = rows.view('<u2' if bits == 16 else '<u4')[:, :tw]
        return clut_colours(px.tobytes(), fmt).reshape(th, tw, 4)
    c5 = st.reg[0xC5]
    shift, mask, start = c5 >> 2 & 31, c5 >> 8 & 255, (c5 >> 16 & 31) * 16
    table = clut_colours(clut, c5 & 3)
    if not len(table):
        return None
    idx = ((idx >> shift) & mask | start) % len(table)
    return table[idx]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dump')
    ap.add_argument('out')
    ap.add_argument('--draws', help='A-B: only these draws')
    ap.add_argument('--only', help='comma list of draws to render (others skipped)')
    ap.add_argument('--owner', action='store_true')
    ap.add_argument('--probe', action='append', default=[], help='X,Y screen point: which draws wrote it')
    args = ap.parse_args()
    lo, hi = map(int, args.draws.split('-')) if args.draws else (0, 1 << 30)
    only = {int(x) for x in args.only.split(',')} if args.only else None
    version, cmds, buf = g.load(args.dump)
    st = g.State()
    vbuf = ibuf = tex = clut = b''
    vpos = ipos = 0
    img = np.zeros((H, W, 3), np.float32)
    zb = np.full((H, W), np.inf, np.float32)
    owner = np.full((H, W), -1, np.int32)
    writers = {tuple(map(int, p.split(','))): [] for p in args.probe}
    ys, xs = np.mgrid[0:H, 0:W].astype(np.float32) + 0.5
    di = 0
    for kind, size, ptr in cmds:
        data = buf[ptr:ptr + size]
        if kind == 0:
            st.init(list(struct.unpack_from(f'<{size // 4}I', data)))
        elif kind == 2: vbuf, vpos = data, 0
        elif kind == 3: ibuf, ipos = data, 0
        elif kind == TEXTURE0: tex = data
        elif kind == CLUT: clut = data
        elif kind == 1:
            for (word,) in struct.iter_unpack('<I', data[:size // 4 * 4]):
                if word >> 24 != 4:
                    st.command(word)
                    continue
                n, prim, vt = word & 0xFFFF, (word >> 16) & 7, st.reg[0x12]
                fields, stride, nw, wt = g.layout(vt)
                verts, _, ib = g.vertices(st, vbuf, vpos, n, ibuf, ipos)
                itype = (vt >> 11) & 3
                idxs = [ibuf[ipos + k] if itype == 1 else struct.unpack_from('<H', ibuf, ipos + 2 * k)[0] if itype == 2 else k
                        for k in range(n)]
                base = vpos
                if itype: ipos += ib
                else: vpos += stride * n
                draw = di
                di += 1
                if not (lo <= draw <= hi) or (only is not None and draw not in only):
                    continue
                through = bool(vt >> 23 & 1)
                # per-vertex uv, colour
                attrs = []
                for i in idxs:
                    b0 = base + i * stride
                    uv = (0.0, 0.0)
                    if 'tc' in fields and b0 + stride <= len(vbuf):
                        o, s = fields['tc']
                        sc = None if through or s == 4 else {1: 128.0, 2: 32768.0}
                        uv = tuple(g.read_num(vbuf, b0 + o + j * s, s, False, sc) for j in range(2))
                    col = st.reg[0x55] & 0xFFFFFF | (st.reg[0x58] & 0xFF) << 24  # material colour, alpha
                    if 'col' in fields and b0 + stride <= len(vbuf):
                        o, s = fields['col']
                        if s == 4:
                            col = struct.unpack_from('<I', vbuf, b0 + o)[0]
                        else:
                            v16 = struct.unpack_from('<H', vbuf, b0 + o)[0]
                            ct = (vt >> 2) & 7
                            r, gg, bb, a = clut_colours(struct.pack('<H', v16), {4: 0, 5: 1, 6: 2}[ct])[0]
                            col = int(r) | int(gg) << 8 | int(bb) << 16 | int(a) << 24
                    attrs.append((uv, col))
                if not through:
                    su, sv = g.f24(st.reg[0x48]) or 1.0, g.f24(st.reg[0x49]) or 1.0
                    ou, ov = g.f24(st.reg[0x4A]), g.f24(st.reg[0x4B])
                    attrs = [((u * su + ou, v * sv + ov), c) for (u, v), c in attrs]
                textured = bool(st.reg[0x1E] & 1)
                timg = texture_image(st, tex, clut) if textured else None
                if timg is not None and through:
                    attrs = [((u / timg.shape[1], v / timg.shape[0]), c) for (u, v), c in attrs]
                zte = bool(st.reg[0x23] & 1) and not through
                zwrite = not (st.reg[0xE7] & 1)
                ate, aref, afunc = bool(st.reg[0x22] & 1), st.reg[0xDB] >> 8 & 255, st.reg[0xDB] & 7
                abe = bool(st.reg[0x21] & 1)
                items = list(zip(verts, attrs))
                for t in g.triangles(prim, items) if prim != 6 else []:
                    pass
                tris = []
                if prim == 6:
                    for k in range(0, len(items) - 1, 2):
                        (p0, a0), (p1, a1) = items[k], items[k + 1]
                        if not (p0 and p1) or isinstance(p0[0], str) or isinstance(p1[0], str): continue
                        (u0, v0), (u1, v1) = a0[0], a1[0]
                        q = [((p0[0], p0[1], 1), ((u0, v0), a1[1])), ((p1[0], p0[1], 1), ((u1, v0), a1[1])),
                             ((p1[0], p1[1], 1), ((u1, v1), a1[1])), ((p0[0], p1[1], 1), ((u0, v1), a1[1]))]
                        tris += [[q[0], q[1], q[2]], [q[0], q[2], q[3]]]
                else:
                    tris = g.triangles(prim, items)
                for t in tris:
                    if not all(p and not isinstance(p[0], str) for p, a in t): continue
                    (x0, y0, w0), (x1, y1, w1), (x2, y2, w2) = [p for p, a in t]
                    minx, maxx = max(int(min(x0, x1, x2)), 0), min(int(max(x0, x1, x2)) + 1, W)
                    miny, maxy = max(int(min(y0, y1, y2)), 0), min(int(max(y0, y1, y2)) + 1, H)
                    if minx >= maxx or miny >= maxy: continue
                    den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
                    if abs(den) < 1e-9: continue
                    X, Y = xs[miny:maxy, minx:maxx], ys[miny:maxy, minx:maxx]
                    l0 = ((y1 - y2) * (X - x2) + (x2 - x1) * (Y - y2)) / den
                    l1 = ((y2 - y0) * (X - x2) + (x0 - x2) * (Y - y2)) / den
                    l2 = 1 - l0 - l1
                    m = (l0 >= 0) & (l1 >= 0) & (l2 >= 0)
                    if not m.any(): continue
                    if through:
                        p0 = p1 = p2 = 1.0
                    else:
                        p0, p1, p2 = 1 / w0, 1 / w1, 1 / w2
                    q = l0 * p0 + l1 * p1 + l2 * p2
                    depth = 1 / q
                    zz = zb[miny:maxy, minx:maxx]
                    if zte: m &= depth <= zz * 1.0005
                    cols = [np.array([a[1] & 255, a[1] >> 8 & 255, a[1] >> 16 & 255, a[1] >> 24 & 255], np.float32)
                            for p, a in t]
                    rgba = (l0[..., None] * p0 * cols[0] + l1[..., None] * p1 * cols[1] + l2[..., None] * p2 * cols[2]) / q[..., None]
                    if timg is not None:
                        uvs = [a[0] for p, a in t]
                        u = (l0 * p0 * uvs[0][0] + l1 * p1 * uvs[1][0] + l2 * p2 * uvs[2][0]) / q
                        v = (l0 * p0 * uvs[0][1] + l1 * p1 * uvs[1][1] + l2 * p2 * uvs[2][1]) / q
                        th, tw = timg.shape[:2]
                        ti = (np.floor(u * tw).astype(np.int64) % tw, np.floor(v * th).astype(np.int64) % th)
                        tx = timg[ti[1], ti[0]].astype(np.float32)
                        rgba = tx * rgba / 255.0
                    if ate:
                        a = rgba[..., 3]
                        ok = {0: a < -1, 1: a >= -1, 2: a == aref, 3: a != aref, 4: a < aref, 5: a <= aref,
                              6: a > aref, 7: a >= aref}[afunc]
                        m &= ok
                    if not m.any(): continue
                    dst = img[miny:maxy, minx:maxx]
                    if abe:
                        al = (rgba[..., 3:4] / 255.0)
                        new = rgba[..., :3] * al + dst * (1 - al)
                    else:
                        new = rgba[..., :3]
                    dst[m] = new[m]
                    if zwrite and zte or (zwrite and not through):
                        zz[m] = depth[m]
                    owner[miny:maxy, minx:maxx][m] = draw
                    for (px, py), lst in writers.items():
                        if minx <= px < maxx and miny <= py < maxy and m[py - miny, px - minx]:
                            if not lst or lst[-1] != draw: lst.append(draw)
    Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).resize((W * 2, H * 2), Image.NEAREST).save(args.out)
    print(f'{di} draws; {args.out}')
    for p, lst in writers.items():
        print('probe', p, 'written by draws', lst)
    if args.owner:
        pal = np.array([[int(c * 255) for c in colorsys.hsv_to_rgb((i * 0.618) % 1, 0.8, 1)] for i in range(max(di, 1))], np.uint8)
        o = np.where(owner[..., None] >= 0, pal[np.maximum(owner, 0)], 0).astype(np.uint8)
        Image.fromarray(o).resize((W * 2, H * 2), Image.NEAREST).save(args.out[:-4] + '.owner.png')


if __name__ == '__main__':
    main()
