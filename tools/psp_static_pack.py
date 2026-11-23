"""Player ISO -> compact static geometry and relocatable GE templates (private game data).

All positions use one model-space quarter-unit lattice, regardless of s8/s16.
Metadata/matrices are floats; vertex streams contain only integers.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import struct
import time

from psp_disc import ISO, DIST
from psp_strips import chunks, iso_files, geometries, join, mesh_triangles, same_triangles, fnv1a
from psp_meshopt import Meshopt

MAGIC = 0x31534750
WORLD_MAGIC = 0x32534750
HEADER = struct.Struct('<8I16f')  # 96 bytes; final two floats reserved
MESH = struct.Struct('<8I')
BATCH = struct.Struct('<6f2I')
STEP = 0.25
MAX_ENTRY = 1 << 20
MAX_ADDED_TRIANGLES = 1024


def source_bytes(g, lit):
    data = bytearray(struct.pack('<7I', g['nverts'], len(g['meshes']), g['strip'], lit,
                                 bool(g['uv']), bool(g['prelit']), bool(lit)))
    data += g['verts'] + g['uv'] + g['prelit']
    if lit:
        data += g['normals']
    for mesh in g['meshes']:
        data += struct.pack('<I', len(mesh)) + struct.pack(f'<{len(mesh)}H', *mesh)
    return bytes(data)


def key_for(data):
    check = 5381
    for b in data:
        check = ((check * 33) ^ b) & 0xffffffff
    return fnv1a(data), check


def align(blob):
    blob += bytes(-len(blob) % 16)
    return len(blob)


def world_batches(triangles, positions):
    """Partition runs at spatial discontinuities while keeping alpha/depth order."""
    centres = [tuple(sum(positions[i][k] for i in t) for k in range(3)) for t in triangles]
    first = 0
    while len(triangles) - first > 64:
        last = min(first + 64, len(triangles) - 32)
        split = max(range(first + 32, last + 1),
                    key=lambda i: math.dist(centres[i - 1], centres[i]))
        yield triangles[first:split]
        first = split
    yield triangles[first:]


def ordered_strip(triangles):
    runs = []
    start = 0
    while start < len(triangles):
        best = []
        first = triangles[start]
        for rotation in range(3):
            strip = list(first[rotation:] + first[:rotation])
            for i, t in enumerate(triangles[start + 1:], 1):
                a, b = strip[-2:]
                if i & 1:
                    a, b = b, a
                if a not in t or t[(t.index(a) + 1) % 3] != b:
                    break
                strip.append(t[(t.index(a) + 2) % 3])
            if len(strip) > len(best):
                best = strip
        runs.append(best)
        start += len(best) - 2
    strip = join(runs)
    if len(strip) < len(triangles) * 3:
        return 4, strip
    return 3, [v for t in triangles for v in t]


def split_surface(mesh, positions, uv, normals, colours, budget, shared_edges):
    """Bisect paired interior edges on a surface identified in the guard census."""
    edges = defaultdict(list)
    for ti, tri in enumerate(mesh):
        for k in range(3):
            a, b = tri[k], tri[(k + 1) % 3]
            key = tuple(sorted((positions[a], positions[b])))
            edges[key].append((ti, a, b, tri[(k + 2) % 3]))
    candidates = sorted(((math.dist(*edge), uses) for edge, uses in edges.items()
                         if len(uses) == 2 and edge not in shared_edges and
                         uses[0][1:3] == uses[1][1:3][::-1]), reverse=True)
    splits = {}
    for _, (left, right) in candidates:
        if len(splits) + 2 > min(len(mesh) // 5, budget) or len(positions) == 65535:
            break
        if any(t[0] in splits for t in (left, right)):
            continue
        mid = len(positions)
        a, b = left[1:3]
        for stream in (positions, uv, normals):
            stream.append(tuple((x + y) * 0.5 for x, y in zip(stream[a], stream[b])))
        colours.append(tuple((x + y + 1) // 2 for x, y in zip(colours[a], colours[b])))
        for ti, a, b, c in (left, right):
            splits[ti] = [(a, mid, c), (mid, b, c)]
    return [piece for ti, triangle in enumerate(mesh) for piece in splits.get(ti, (triangle,))]


def materials(data, geometry):
    start = geometry['offset']
    end = start + geometry['size']
    for kind, offset, length, _ in chunks(data, start, end):
        if kind != 8:
            continue
        children = iter(chunks(data, offset, offset + length))
        kind, offset, length, _ = next(children, (0, 0, 0, 0))
        if kind != 1 or length < 4 or length % 4:
            return []
        refs = struct.unpack_from(f'<{length // 4}i', data, offset)
        if refs[0] != len(refs) - 1:
            return []
        names = []
        for kind, offset, length, _ in children:
            if kind != 7:
                continue
            name = ''
            for kind, offset, length, _ in chunks(data, offset, offset + length):
                if kind == 6:
                    name = next((data[o:o+n].split(b'\0', 1)[0].decode('ascii', 'replace').lower()
                                 for t, o, n, _ in chunks(data, offset, offset + length) if t == 2), '')
                    break
            names.append(name)
        slots = []
        new = iter(names)
        for i in refs[1:]:
            slots.append(next(new, '') if i < 0 else slots[i] if i < len(slots) else '')
        return [slots[i] if i < len(slots) else '' for i in geometry['mesh_materials']]
    return []


def write_world(blob, meshes, packed, points, base_type):
    batches = [list(world_batches(t, points)) for t in meshes]
    table = len(blob)
    blob += bytes(sum(map(len, batches)) * BATCH.size)
    vertex = align(blob)
    blob += b''.join(packed)
    vtype = base_type | 2 << 11
    first = indices = words = 0
    for mi, group in enumerate(batches):
        commands = []
        start = align(blob)
        count = 0
        for triangles in group:
            prim, order = ordered_strip(triangles)
            bounds = list(zip(*(points[i] for i in order)))
            off = len(blob)
            blob += struct.pack(f'<{len(order)}H', *order)
            command = prim << 16 | len(order)
            BATCH.pack_into(blob, table + first * BATCH.size, *map(min, bounds), *map(max, bounds), off, command)
            commands.extend((0x12000000 | vtype, 0x10000000, 0x01000000 | vertex,
                             0x10000000, 0x02000000 | off, 0x04000000 | command))
            first += 1
            count += len(order)
        co = align(blob)
        commands.append(0x0b000000)
        blob += struct.pack(f'<{len(commands)}I', *commands)
        MESH.pack_into(blob, HEADER.size + mi * MESH.size, vertex, len(packed), start,
                       count, co, len(commands), first - len(group), vtype)
        indices += count
        words += len(commands)
    struct.pack_into('<2I', blob, 88, table, first)
    return dict(vertices=len(packed), indices=indices, batches=first, ge_words=words)


def make_entry(g, lit, optimizer, error=0.125, *, world=False, split=(), budget=0):
    nv, nm = g['nverts'], len(g['meshes'])
    if g['skin'] or not 1 <= nv <= 65535 or not 1 <= nm <= 32 or g['strip'] not in (0, 1):
        raise ValueError('skin, unsupported topology or geometry size')
    if len(g['verts']) != nv * 12 or (lit and len(g['normals']) != nv * 12):
        raise ValueError('missing vertex stream')
    positions = list(struct.iter_unpack('<3f', g['verts']))
    uv = list(struct.iter_unpack('<2f', g['uv'])) if g['uv'] else [(0, 0)] * nv
    normals = list(struct.iter_unpack('<3f', g['normals'])) if lit else [(0, 0, 0)] * nv
    colours = list(struct.iter_unpack('<4B', g['prelit'])) if g['prelit'] else [(0, 0, 0, 255) if lit else (128, 128, 128, 255)] * nv
    if len(uv) != nv or len(colours) != nv or (not world and any(c[3] != 255 for c in colours)):
        raise ValueError('translucent prelight or truncated attributes')
    if not all(math.isfinite(v) for stream in (positions, uv, normals) for p in stream for v in p):
        raise ValueError('non-finite attribute')
    if any(not 0 <= i < nv for mesh in g['meshes'] for i in mesh):
        raise ValueError('index outside vertex array')
    meshes = [mesh_triangles(mesh, g['strip']) for mesh in g['meshes']]
    input_triangles = 0
    if world:
        meshes = [[t for t in mesh if len(set(t)) == 3] for mesh in meshes]
        if not all(meshes):
            raise ValueError('empty mesh')
        input_triangles = sum(map(len, meshes))
        budget = min(budget, input_triangles // 5)
        for mi in split:
            shared_edges = {tuple(sorted((positions[a], positions[b])))
                            for other, triangles in enumerate(meshes) if other != mi
                            for t in triangles for a, b in zip(t, t[1:] + t[:1])}
            original = len(meshes[mi])
            meshes[mi] = split_surface(meshes[mi], positions, uv, normals, colours, budget, shared_edges)
            budget -= len(meshes[mi]) - original
    lo = [min(p[k] for p in positions) for k in range(3)]
    hi = [max(p[k] for p in positions) for k in range(3)]
    step = STEP
    if world and max(b - a for a, b in zip(lo, hi)) > 65532 * STEP:
        step *= 2
    origin = [round((a + b) / (2 * step)) for a, b in zip(lo, hi)]
    centre = [v * step for v in origin]
    quantized = [tuple(round(p[k] / step) - origin[k] for k in range(3)) for p in positions]
    # Leave one signed unit for outward-rounded boxes.
    extent = max(abs(v) for p in quantized for v in p)
    bits = 8 if not world and extent <= 126 else 16 if extent <= 32766 else 0
    if not bits:
        raise ValueError('outside integer grid; no float vertex fallback')
    scale = step * (128 if bits == 8 else 32768)
    ul = [min(p[k] for p in uv) for k in range(2)]
    uh = [max(p[k] for p in uv) for k in range(2)]
    if any(abs(v) > (256 if world else 16) for p in uv for v in p) or any(b - a > (64 if world else 8) for a, b in zip(ul, uh)):
        raise ValueError('UV range needs runtime rebasing')
    decode = [(b - a) * 32768 / 32767 if b > a else 1 for a, b in zip(ul, uh)] + ul
    packed, attributes = [], []
    for p, tex, n, c in zip(quantized, uv, normals, colours):
        t = [max(0, min(32767, round((tex[k] - ul[k]) * 32768 / decode[k]))) for k in range(2)]
        colour = tuple(min(255, v * 2) for v in c[:3]) + (c[3],)
        v = struct.pack('<2H4B', *t, *colour)
        length = math.sqrt(sum(x * x for x in n))
        normal = tuple(x / length for x in n) if length else (0, 0, 0)
        if lit:
            v += struct.pack('<4b', *(max(-127, min(127, round(x * 127))) for x in normal), 0)
        v += struct.pack('<3b' if bits == 8 else '<3h', *p)
        v += bytes(-len(v) % 4)
        packed.append(v)
        attributes.append((*tex, *normal, *(v / 255 for v in colour)))
    stride = len(packed[0])
    base_type = 2 | (7 << 2) | ((1 << 5) if lit else 0) | ((1 if bits == 8 else 2) << 7)
    blob = bytearray(HEADER.size + nm * MESH.size)
    points = [tuple(centre[k] + q[k] * step for k in range(3)) for q in quantized]
    if world:
        lo, hi = [list(f(axis) for axis in zip(*points)) for f in (min, max)]
    HEADER.pack_into(blob, 0, WORLD_MAGIC if world else MAGIC, nv, nm, lit, bits, stride, base_type, HEADER.size,
                     *centre, scale, *decode, *lo, *hi, 0, 0)
    if world:
        for mesh in meshes:
            for a, b, c in mesh:
                check_face(positions, quantized, a, b, c)
        stats = write_world(blob, meshes, packed, points, base_type)
        if len(blob) > MAX_ENTRY:
            raise ValueError('entry exceeds runtime cache budget')
        triangles = sum(map(len, meshes))
        return bytes(blob), dict(stats, bits=bits, input_triangles=input_triangles, output_triangles=triangles,
                                 max_position_error=max(map(math.dist, positions, points)), simplification_error=0)
    # Locks span the whole geometry: coincident seams and material boundaries cannot collapse.
    uses, coincident = defaultdict(set), defaultdict(list)
    for mi, mesh in enumerate(g['meshes']):
        for i in mesh:
            uses[i].add(mi)
    for i, p in enumerate(positions):
        coincident[p].append(i)
    seam = {p for p, group in coincident.items()
            if len({attributes[i] for i in group}) > 1 or len(set().union(*(uses[i] for i in group))) > 1}
    locks = [int(p in seam) for p in positions]
    stats = Counter(bits=bits, input_triangles=0, output_triangles=0, vertices=0, indices=0, indexed_meshes=0)
    stats['max_position_error'] = max(math.dist(p, tuple(centre[k] + q[k] * STEP for k in range(3))) for p, q in zip(positions, quantized))
    stats['simplification_error'] = 0.0
    for mi, triangles in enumerate(meshes):
        if not triangles:
            raise ValueError('empty mesh')
        # Deduplicate only identical source attributes, before quantization changes their values.
        remap, ids = {}, {}
        for i in dict.fromkeys(v for t in triangles for v in t):
            key = (positions[i], attributes[i])
            ids[i] = remap.setdefault(key, i)
        triangles = [tuple(ids[i] for i in t) for t in triangles]
        triangles = [t for t in triangles if len(set(t)) == 3]
        if not triangles:
            raise ValueError('mesh contains only degenerate triangles')
        stats['input_triangles'] += len(triangles)
        triangles, strip, achieved = optimizer.optimize(triangles, positions, attributes, locks, error)
        if not same_triangles(triangles, [strip]):
            raise ValueError('stripifier changed triangles or winding')
        if achieved > error + 1e-5:
            raise ValueError('simplifier exceeded error bound')
        for a, b, c in triangles:
            check_face(positions, quantized, a, b, c)
        stats['simplification_error'] = max(stats['simplification_error'], achieved)
        stats['output_triangles'] += len(triangles)
        order = strip if len(strip) < len(triangles) * 3 else [v for t in triangles for v in t]
        prim = 4 if len(strip) < len(triangles) * 3 else 3
        if len(order) > 65535:
            raise ValueError('mesh exceeds one GE PRIM')
        compact = list(dict.fromkeys(order))
        mapping = {v: i for i, v in enumerate(compact)}
        flat = len(order) * stride <= len(compact) * stride + len(order) * 2
        vo = align(blob)
        blob += b''.join(packed[v] for v in (order if flat else compact))
        ni, out_nv = len(order), len(order) if flat else len(compact)
        io = 0
        if not flat:
            io = align(blob)
            blob += struct.pack(f'<{ni}H', *(mapping[v] for v in order))
        bo = align(blob)
        bounds = [tuple((math.ceil if high else math.floor)((v - centre[k]) / scale * 32768)
                       for k, v in enumerate(hi if high else lo)) for high in (False, True)]
        for corner in range(8):
            blob += struct.pack('<3h', *(bounds[corner >> k & 1][k] for k in range(3)))
        co = align(blob)
        vt = base_type | (0 if flat else 2 << 11)
        words = [0x12000100, 0x10000000, 0x01000000 | bo, 0x07000008, 0x10000000, 0,
                 0x12000000 | vt, 0x10000000, 0x01000000 | vo]
        if not flat:
            words += [0x10000000, 0x02000000 | io]
        words += [0x04000000 | prim << 16 | ni, 0x0b000000]
        words[5] = 0x09000000 | (co + (len(words) - 1) * 4)
        blob += struct.pack(f'<{len(words)}I', *words)
        MESH.pack_into(blob, HEADER.size + mi * MESH.size, vo, out_nv, io, ni, co, len(words), prim, vt)
        stats['vertices'] += out_nv
        stats['indices'] += 0 if flat else ni
        stats['indexed_meshes'] += not flat
    if len(blob) > MAX_ENTRY:
        raise ValueError('entry exceeds runtime cache budget')
    return bytes(blob), dict(stats)


def check_face(positions, quantized, a, b, c):
    def face(stream):
        u = [stream[b][k] - stream[a][k] for k in range(3)]
        v = [stream[c][k] - stream[a][k] for k in range(3)]
        return (u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0])
    before, after = face(positions), face(quantized)
    if sum(v*v for v in before) > 0 and sum(x*y for x, y in zip(before, after)) <= 0:
        raise ValueError('integer quantization collapses or flips a triangle')


def write_pack(path, entries):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    offset = 16 + len(entries) * 20
    temp = path.with_suffix('.tmp')
    with temp.open('wb') as f:
        f.write(struct.pack('<4I', 0x54533450, 1, len(entries), offset))
        for key, blob in sorted(entries.items()):
            f.write(struct.pack('<5I', *key, offset, len(blob), fnv1a(blob)))
            offset += len(blob)
        for _, blob in sorted(entries.items()):
            f.write(blob)
    temp.replace(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--iso', type=Path, default=ISO)
    ap.add_argument('--out', type=Path, default=DIST / 'p4static.bin')
    ap.add_argument('--library', type=Path)
    ap.add_argument('--error', type=float, default=0.125, help='absolute simplification error; 0 is exact simplification')
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--path', action='append', help='Restrict to matching ISO paths, repeatable')
    ap.add_argument('--world', action='store_true', help='Build ordered world batches for the selected field/arena assets')
    ap.add_argument('--split-texture', action='append', default=[], help='Bisect interior edges only on a named problem surface')
    args = ap.parse_args()
    if not math.isfinite(args.error) or not 0 <= args.error <= 0.125 or args.limit < 0:
        ap.error('error must be between 0 and 0.125; limit must be nonnegative')
    if args.world and not args.path:
        ap.error('--world requires at least one --path selecting a field or arena')
    if args.split_texture and not args.world:
        ap.error('--split-texture requires --world')
    split_names = {name.lower() for name in args.split_texture}
    remaining = MAX_ADDED_TRIANGLES
    optimizer = None if args.world else Meshopt(args.library)
    seen, entries, identities, reports, skipped = set(), {}, {}, [], Counter()
    started = time.monotonic()
    reported = 0
    for name, data in iso_files(args.iso):
        if args.path and not any(path.upper() in name.upper() for path in args.path):
            continue
        for g in geometries(data):
            if g['skin']:
                continue
            names = materials(data, g) if split_names else []
            split = [i for i, name in enumerate(names) if name in split_names]
            for lit in (0, 1) if g['normals'] else (0,):
                try:
                    source = source_bytes(g, lit)
                    digest = hashlib.sha256(source).digest()
                    if digest in seen:
                        continue
                    seen.add(digest)
                    key = key_for(source)
                    if key in identities and identities[key] != digest:
                        raise RuntimeError('static pack identity collision')
                    identities[key] = digest
                    blob, stats = make_entry(g, lit, optimizer, args.error, world=args.world, split=split, budget=remaining)
                    entries[key] = blob
                    reports.append(dict(path=name, key=list(key), lit=lit, bytes=len(blob), **stats))
                    remaining -= stats['output_triangles'] - stats['input_triangles'] if args.world else 0
                except (ValueError, struct.error) as e:
                    skipped[str(e)] += 1
            if args.limit and len(reports) >= args.limit:
                break
        if len(reports) // 100 > reported:
            print(f'{len(reports)} entries, {time.monotonic() - started:.1f}s', flush=True)
            reported = len(reports) // 100
        if args.limit and len(reports) >= args.limit:
            break
    if args.world and not entries:
        ap.error('no world entries matched or passed validation')
    write_pack(args.out, entries)
    report = dict(version=1, world=args.world, meshoptimizer=None if args.world else 'v0.25',
                  position_step=STEP, simplification_error=0 if args.world else args.error,
                  split_textures=sorted(split_names), added_triangles=MAX_ADDED_TRIANGLES - remaining,
                  entries=reports, skipped=dict(skipped), bytes=args.out.stat().st_size)
    args.out.with_suffix('.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(f'{args.out}: {len(entries)} entries, {report["bytes"]} bytes, {time.monotonic() - started:.1f}s')


if __name__ == '__main__':
    main()
