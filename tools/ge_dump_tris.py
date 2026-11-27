"""Find suspicious triangles in a PPSSPP GE dump (PPSSPPGE v6, the .ppdmp a PSP GE-dump plugin or PPSSPP writes).

  python -I tools/ge_dump_tris.py DUMP.ppdmp [--png OUT.png] [--top 20]

No rendering: it walks the recorded commands, keeps the GE state (vertex type, world/view/projection/bone matrices,
viewport, offset), decodes every PRIM's vertices and maps them to screen pixels the way the GE does. It reports the
draws whose triangles reach far outside the screen, have vertices behind the camera (w <= 0) or non-finite values,
and draws a wireframe of the frame (grey) with the --top worst draws in colour, so a "big triangle" artefact can be
tied to a draw (its list position, vertex type, matrices). Skinned draws use the recorded bone matrices.

Format (PPSSPP GPU/Debugger/Record.h): "PPSSPPGE", u32 version, 12-byte game id, u32 command count, u32 pushbuf
size, then two zstd blocks (u32 compressed size each): the commands {u8 type, u32 size, u32 ptr} and the push buffer.
"""
import argparse
import math
import struct
from compression import zstd

INIT, REGISTERS, VERTICES, INDICES = 0, 1, 2, 3


def f24(word):
    return struct.unpack('<f', struct.pack('<I', (word & 0xFFFFFF) << 8))[0]


def load(path):
    data = open(path, 'rb').read()
    assert data[:8] == b'PPSSPPGE', 'not a PPSSPP GE dump'
    version, = struct.unpack_from('<I', data, 8)
    count, bufsize = struct.unpack_from('<II', data, 24)
    at = 32
    size, = struct.unpack_from('<I', data, at)
    commands = zstd.decompress(data[at + 4:at + 4 + size])
    at += 4 + size
    size, = struct.unpack_from('<I', data, at)
    pushbuf = zstd.decompress(data[at + 4:at + 4 + size])
    assert len(commands) == count * 9, (len(commands), count)
    assert len(pushbuf) == bufsize, (len(pushbuf), bufsize)
    return version, [struct.unpack_from('<BII', commands, i * 9) for i in range(count)], pushbuf


class State:
    def __init__(self):
        self.reg = [0] * 256
        self.world = [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]
        self.view = list(self.world)
        self.proj = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
        self.bones = [list(self.world) for _ in range(8)]
        self.wi = self.vi = self.pi = self.bi = 0

    def init(self, words):
        # v6 INIT: the GE state as a command list (registers, then the matrices as data commands 0x2B bones,
        # 0x3B world, 0x3D view, 0x3F projection, 0x41 texgen); a data group that follows no index command starts at 0
        previous = None
        for word in words:
            op = word >> 24
            if op in (0x2B, 0x3B, 0x3D, 0x3F) and previous not in (op, op - 1):
                self.bi = self.wi = self.vi = self.pi = 0
            previous = op
            self.command(word)

    def command(self, word):
        op, arg = word >> 24, word & 0xFFFFFF
        if op == 0x3A: self.wi = arg & 0xF
        elif op == 0x3B:
            if self.wi < 12: self.world[self.wi] = f24(arg)
            self.wi += 1
        elif op == 0x3C: self.vi = arg & 0xF
        elif op == 0x3D:
            if self.vi < 12: self.view[self.vi] = f24(arg)
            self.vi += 1
        elif op == 0x3E: self.pi = arg & 0xF
        elif op == 0x3F:
            if self.pi < 16: self.proj[self.pi] = f24(arg)
            self.pi += 1
        elif op == 0x2A: self.bi = arg & 0x7F
        elif op == 0x2B:
            if self.bi < 96: self.bones[self.bi // 12][self.bi % 12] = f24(arg)
            self.bi += 1
        else:
            self.reg[op] = word


def mul43(m, v):  # PSP 4x3 matrices are column-major: m[0..2] is column 0
    x, y, z = v
    return (m[0] * x + m[3] * y + m[6] * z + m[9], m[1] * x + m[4] * y + m[7] * z + m[10],
            m[2] * x + m[5] * y + m[8] * z + m[11])


def mul44(m, v):
    x, y, z = v
    return tuple(m[i] * x + m[4 + i] * y + m[8 + i] * z + m[12 + i] for i in range(4))


SIZES = {'tc': [0, 1, 2, 4], 'col': [0, 0, 0, 0, 2, 2, 2, 4], 'nrm': [0, 1, 2, 4], 'pos': [0, 1, 2, 4],
         'w': [0, 1, 2, 4]}


def layout(vtype):
    tc, col, nrm, pos, wt = vtype & 3, (vtype >> 2) & 7, (vtype >> 5) & 3, (vtype >> 7) & 3, (vtype >> 9) & 3
    nw = ((vtype >> 14) & 7) + 1 if wt else 0
    off, align = 0, 1
    fields = {}
    def put(name, size, n):
        nonlocal off, align
        if size:
            off = (off + size - 1) // size * size
            fields[name] = (off, size)
            off += size * n
            align = max(align, size)
    put('w', SIZES['w'][wt], nw)
    put('tc', SIZES['tc'][tc], 2)
    put('col', SIZES['col'][col], 1)
    put('nrm', SIZES['nrm'][nrm], 3)
    put('pos', SIZES['pos'][pos], 3)
    stride = (off + align - 1) // align * align
    return fields, stride, nw, wt


def read_num(buf, at, size, signed, scale_int):
    if size == 4:
        return struct.unpack_from('<f', buf, at)[0]
    fmt = {1: 'b' if signed else 'B', 2: 'h' if signed else 'H'}[size]
    v = struct.unpack_from('<' + fmt, buf, at)[0]
    return v / scale_int[size] if scale_int else v


def vertices(state, vbuf, vpos, n, ibuf, ipos):
    vtype = state.reg[0x12]
    through = bool(vtype & (1 << 23))
    fields, stride, nw, wt = layout(vtype)
    itype = (vtype >> 11) & 3
    indices = []
    for k in range(n):
        if itype == 1: indices.append(ibuf[ipos + k])
        elif itype == 2: indices.append(struct.unpack_from('<H', ibuf, ipos + 2 * k)[0])
        else: indices.append(k)
    out = []
    for i in indices:
        base = vpos + i * stride
        if base + stride > len(vbuf):
            out.append(None)
            continue
        po, ps = fields['pos']
        if through:
            p = tuple(read_num(vbuf, po + base + j * ps, ps, True, None) for j in range(3))
            out.append((p[0], p[1], 1.0))
            continue
        p = tuple(read_num(vbuf, po + base + j * ps, ps, True, {1: 128.0, 2: 32768.0}) for j in range(3))
        if nw:
            wo, wsz = fields['w']
            ws = [read_num(vbuf, wo + base + j * wsz, wsz, False, {1: 128.0, 2: 32768.0}) for j in range(nw)]
            acc = [0.0, 0.0, 0.0]
            for j, w in enumerate(ws):
                q = mul43(state.bones[j], p)
                acc = [a + w * b for a, b in zip(acc, q)]
            p = tuple(acc)
        wv = mul43(state.view, mul43(state.world, p))
        c = mul44(state.proj, wv)
        if not all(math.isfinite(x) for x in c):
            out.append(('nan', c))
            continue
        if c[3] <= 0:
            out.append(('behind', c))
            continue
        nx, ny = c[0] / c[3], c[1] / c[3]
        sx = f24(state.reg[0x42]) * nx + f24(state.reg[0x45]) - (state.reg[0x4C] & 0xFFFF) / 16
        sy = f24(state.reg[0x43]) * ny + f24(state.reg[0x46]) - (state.reg[0x4D] & 0xFFFF) / 16
        out.append((sx, sy, c[3]))
    return out, stride, (2 if itype == 2 else 1) * n


def triangles(prim, verts):
    if prim == 3: return [verts[i:i + 3] for i in range(0, len(verts) - 2, 3)]
    if prim == 4: return [verts[i:i + 3] for i in range(len(verts) - 2)]
    if prim == 5: return [[verts[0], verts[i], verts[i + 1]] for i in range(1, len(verts) - 1)]
    if prim == 6:  # rectangles: two corners
        out = []
        for i in range(0, len(verts) - 1, 2):
            a, b = verts[i], verts[i + 1]
            if isinstance(a, tuple) and isinstance(b, tuple) and len(a) == 3 and len(b) == 3 and \
                    not isinstance(a[0], str) and not isinstance(b[0], str):
                out += [[a, (b[0], a[1], a[2]), b], [a, b, (a[0], b[1], a[2])]]
        return out
    return []


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dump')
    ap.add_argument('--png')
    ap.add_argument('--top', type=int, default=15)
    args = ap.parse_args()
    version, commands, buf = load(args.dump)
    st = State()
    vbuf, vpos, ibuf, ipos = b'', 0, b'', 0
    draws = []
    for ci, (kind, size, ptr) in enumerate(commands):
        data = buf[ptr:ptr + size]
        if kind == INIT:
            st.init(list(struct.unpack_from(f'<{size // 4}I', data)))
        elif kind == VERTICES:
            vbuf, vpos = data, 0
        elif kind == INDICES:
            ibuf, ipos = data, 0
        elif kind == REGISTERS:
            for (word,) in struct.iter_unpack('<I', data[:size // 4 * 4]):
                if word >> 24 != 0x04:
                    st.command(word)
                    continue
                n, prim = word & 0xFFFF, (word >> 16) & 7
                verts, stride, ibytes = vertices(st, vbuf, vpos, n, ibuf, ipos)
                if (st.reg[0x12] >> 11) & 3:
                    ipos += ibytes
                else:
                    vpos += stride * n
                bad = [v for v in verts if v is None or isinstance(v[0], str)]
                good = [v for v in verts if v is not None and not isinstance(v[0], str)]
                ext = 0.0
                for t in triangles(prim, verts):
                    if all(v is not None and not isinstance(v[0], str) for v in t):
                        xs, ys = [v[0] for v in t], [v[1] for v in t]
                        e = max(max(xs) - min(xs), max(ys) - min(ys))
                        far = max(max(abs(x - 240) for x in xs), max(abs(y - 136) for y in ys))
                        ext = max(ext, e if far > 600 else e * 0.25)
                draws.append(dict(index=len(draws), command=ci, prim=prim, n=n, vtype=st.reg[0x12],
                                  through=bool(st.reg[0x12] >> 23 & 1), bad=len(bad),
                                  kinds=sorted({v[0] if v else 'oob' for v in bad}), extent=ext, verts=verts))
    print(f'version {version}: {len(commands)} commands, {len(draws)} draws, '
          f'{sum(d["n"] for d in draws)} vertices')
    worst = sorted(draws, key=lambda d: (d['bad'] > 0, d['extent']), reverse=True)[:args.top]
    for d in worst:
        good = [v for v in d['verts'] if v is not None and not isinstance(v[0], str)]
        box = (min(v[0] for v in good), min(v[1] for v in good), max(v[0] for v in good), max(v[1] for v in good)) if good else None
        print(f"draw {d['index']:4d} cmd {d['command']:4d} prim {d['prim']} n {d['n']:5d} vtype {d['vtype'] & 0xFFFFFF:06x}"
              f"{' through' if d['through'] else ''} bad {d['bad']} {d['kinds']} extent {d['extent']:.0f}"
              f" box {tuple(round(x) for x in box) if box else None}")
    if args.png:
        from PIL import Image, ImageDraw
        img = Image.new('RGB', (480 * 2, 272 * 2), (0, 0, 0))
        g = ImageDraw.Draw(img)
        def pt(v): return (v[0] * 2, v[1] * 2)
        colours = [(255, 60, 60), (60, 255, 60), (80, 140, 255), (255, 220, 0), (255, 0, 255), (0, 255, 255)]
        top = {d['index']: colours[i % len(colours)] for i, d in enumerate(worst[:6])}
        for d in sorted(draws, key=lambda d: d['index'] in top):
            colour = top.get(d['index'], (70, 70, 70))
            for t in triangles(d['prim'], d['verts']):
                if all(v is not None and not isinstance(v[0], str) for v in t):
                    if max(abs(c) for v in t for c in v[:2]) < 1e5:
                        g.polygon([pt(v) for v in t], outline=colour)
        g.rectangle([0, 0, 959, 543], outline=(120, 120, 120))
        img.save(args.png)
        print('wireframe:', args.png, '(2x scale; the top 6 draws in red, green, blue, yellow, magenta, cyan)')


if __name__ == '__main__':
    main()
