"""Integer geometry, shared-grid seams, simplification and the production GE packet validator."""
import ctypes as C
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

from psp_meshopt import Meshopt
from psp_static_pack import BATCH, HEADER, MESH, STEP, make_entry, source_bytes, key_for, write_pack
from psp_strips import canon, mesh_triangles

ROOT = Path(__file__).resolve().parents[1]


def geometry(points, meshes=None, lit=False):
    return dict(nverts=len(points), verts=b''.join(struct.pack('<3f', *p) for p in points),
                uv=b'', prelit=b'', normals=struct.pack('<3f', 0, 1, 0) * len(points) if lit else b'',
                meshes=meshes or [(0, 1, 2, 0, 2, 3)], strip=0, skin=None)


def decoded(blob, mi=0):
    h = HEADER.unpack_from(blob)
    vo, nv, io, count, co, nw, prim, vt = MESH.unpack_from(blob, HEADER.size + mi * MESH.size)
    offset = 8 + h[3] * 4
    fmt = '<3b' if h[4] == 8 else '<3h'
    q = [struct.unpack_from(fmt, blob, vo + i * h[5] + offset) for i in range(nv)]
    points = [tuple(h[8 + k] + p[k] * STEP for k in range(3)) for p in q]
    ix = list(struct.unpack_from(f'<{count}H', blob, io)) if io else list(range(count))
    return h, points, ix, prim


class StaticPackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.optimizer = Meshopt()
        cls.temp = tempfile.TemporaryDirectory()
        path = Path(cls.temp.name)
        code = path / 'check.c'
        code.write_text('''#include "static_pack.h"
__declspec(dllexport) int validate(const void *p,unsigned n){return p4_static_validate(p,n);}
__declspec(dllexport) void relocate(void *p,uint32_t a){p4_static_relocate(p,a);}
__declspec(dllexport) void hash(uint32_t *h,const void *p,unsigned n){p4_static_hash(h,p,n);}
__declspec(dllexport) int screen(const float *lo,const float *hi,const float m[4][4],float near,float area,int near_clip){
    return p4_static_screen(lo,hi,m,near,8,area,near_clip);
}
''')
        compiler = shutil.which('gcc')
        if not compiler:
            raise RuntimeError('Static packet checks require a host C compiler')
        dll = path / 'check.dll'
        subprocess.run([compiler, '-std=c11', '-O2', '-shared', '-I' + str(ROOT / 'port'), str(code), '-o', str(dll)], check=True)
        cls.native = C.CDLL(str(dll))
        cls.native.validate.argtypes = [C.c_void_p, C.c_uint]
        cls.native.relocate.argtypes = [C.c_void_p, C.c_uint32]
        cls.native.hash.argtypes = [C.POINTER(C.c_uint32), C.c_void_p, C.c_uint]
        cls.native.screen.argtypes = [C.POINTER(C.c_float)] * 3 + [C.c_float] * 2 + [C.c_int]

    @classmethod
    def tearDownClass(cls):
        # Windows retains the loaded test DLL until process exit.
        import _ctypes
        _ctypes.FreeLibrary(cls.native._handle)
        cls.temp.cleanup()

    def entry(self, g, lit=0, error=0.125):
        blob, stats = make_entry(g, lit, self.optimizer, error)
        self.assertEqual(self.native.validate(blob, len(blob)), 1)
        return blob, stats

    def test_s8_s16_integer_formats_bounds_and_relocation(self):
        for size, bits in ((10, 8), (1000, 16)):
            for lit in (0, 1):
                g = geometry([(0,0,0), (size,0,0), (size,0,size), (0,0,size)], lit=bool(lit))
                blob, stats = self.entry(g, lit)
                h, points, _, _ = decoded(blob)
                self.assertEqual(h[4], bits)
                self.assertEqual((h[6] >> 7) & 3, 1 if bits == 8 else 2)
                self.assertEqual(h[6] & 3, 2)  # UV16, never float
                self.assertEqual((h[6] >> 5) & 3, lit)
                self.assertEqual(set(points), set(struct.iter_unpack('<3f', g['verts'])))
                mutable = C.create_string_buffer(blob)
                self.native.relocate(mutable, 0x09fff000)
                vo,nv,io,count,co,nw,prim,vt = MESH.unpack_from(blob, HEADER.size)
                words = struct.unpack_from(f'<{nw}I', mutable.raw, co)
                address = lambda a,b: ((a & 0xf0000) << 8) | (b & 0xffffff)
                self.assertEqual(address(words[7], words[8]), 0x09fff000 + vo)
                self.assertEqual(address(words[4], words[5]), 0x09fff000 + co + (nw - 1) * 4)

    def test_shared_vertices_use_absolute_grid_across_different_centres_and_formats(self):
        shared = (0.125, 0, 0.125)  # ties previously rounded differently around odd/even origins
        points = [shared, (4.25,0,0.125), (4.25,0,4.125), (0.125,0,4.125)]
        a, _ = self.entry(geometry(points), error=0)
        b, _ = self.entry(geometry(points + [(1000,0,1000)], meshes=[(0,1,2,0,2,3)]), error=0)
        self.assertEqual(decoded(a)[0][4], 8)
        self.assertEqual(decoded(b)[0][4], 16)
        self.assertEqual(set(decoded(a)[1]), set(decoded(b)[1]))

    def test_simplification_preserves_borders_and_winding(self):
        n = 12
        points = [(x, 0, z) for z in range(n + 1) for x in range(n + 1)]
        triangles = []
        for z in range(n):
            for x in range(n):
                a = z * (n + 1) + x
                triangles += [a,a+1,a+n+2,a,a+n+2,a+n+1]
        g = geometry(points, [triangles], lit=True)
        g['uv'] = b''.join(struct.pack('<2f', x/n, z/n) for x,_,z in points)
        blob, stats = self.entry(g, 1)
        self.assertLess(stats['output_triangles'], stats['input_triangles'])
        self.assertLessEqual(stats['simplification_error'], 0.125)
        _, out, _, _ = decoded(blob)
        boundary = {p for p in points if p[0] in (0,n) or p[2] in (0,n)}
        self.assertTrue(boundary <= set(out))

    def test_wrong_commands_indices_and_truncation_are_rejected(self):
        blob, _ = self.entry(geometry([(0,0,0),(10,0,0),(10,0,10),(0,0,10)]))
        self.assertEqual(self.native.validate(blob, len(blob) - 1), 0)
        mesh = MESH.unpack_from(blob, HEADER.size)
        for offset, value in ((16,32), (mesh[4],0x0c000000), (mesh[4]+20,0x09000000), (HEADER.size,0xfffffff0)):
            bad = bytearray(blob)
            struct.pack_into('<I', bad, offset, value)
            self.assertEqual(self.native.validate(bytes(bad), len(bad)), 0)
        for i in range(0, len(blob), 4):
            self.assertEqual(self.native.validate(blob, i), 0)

    def test_hash_matches_runtime_and_writer_offsets(self):
        g = geometry([(0,0,0),(10,0,0),(10,0,10),(0,0,10)])
        source = source_bytes(g, 0)
        key = (C.c_uint32 * 2)(2166136261,5381)
        self.native.hash(key, source, len(source))
        self.assertEqual(tuple(key), key_for(source))
        blob, _ = self.entry(g)
        p = Path(self.temp.name) / 'pack.bin'
        write_pack(p, {tuple(key):blob})
        head = struct.unpack_from('<4I', p.read_bytes())
        self.assertEqual(head[2:], (1,36))
        self.assertEqual(p.read_bytes()[36:], blob)

    def test_world_packets_preserve_order_and_validate_bounds(self):
        points = [(x, y, 10) for x in range(80) for y in (0, 1)]
        strip = tuple(range(len(points)))
        g = geometry(points, [strip], lit=True)
        g['strip'] = 1
        g['prelit'] = bytes((0, 0, 0, 127)) * len(points)
        blob, stats = make_entry(g, 1, None, world=True)
        self.assertEqual(self.native.validate(blob, len(blob)), 1)
        self.assertGreater(stats['batches'], 1)
        self.assertEqual(stats['input_triangles'], stats['output_triangles'])
        table, batches = struct.unpack_from('<2I', blob, 88)
        draw = []
        for i in range(batches):
            *_, index, command = BATCH.unpack_from(blob, table + i * BATCH.size)
            ix = struct.unpack_from(f'<{command & 65535}H', blob, index)
            draw += mesh_triangles(ix, command >> 16 == 4)
        self.assertEqual([canon(*t) for t in draw], [canon(*t) for t in mesh_triangles(strip, 1)])
        bad = bytearray(blob)
        struct.pack_into('<f', bad, table, 10000)
        self.assertEqual(self.native.validate(bytes(bad), len(bad)), 0)
        self.assertEqual(self.native.validate(blob, len(blob) - 1), 0)
        mutable = C.create_string_buffer(blob)
        self.native.relocate(mutable, 0x09ffff80)
        mesh = MESH.unpack_from(blob, HEADER.size)
        word = struct.unpack_from('<6I', mutable.raw, mesh[4])
        self.assertEqual(((word[3] & 0xf0000) << 8) | (word[4] & 0xffffff), 0x09ffff80 + mesh[2])

    def test_world_screen_distinguishes_edge_and_guard_crossings(self):
        p = (C.c_float * 16)(1,0,0,0, 0,1,0,0, 0,0,1,1, 0,0,0,0)
        v = C.c_float * 3
        for lo, hi, verdict in (
            ((-1, -1, 10), (1, 1, 20), 1),
            ((-1, -1, 10), (20, 1, 20), 2),
            ((-1, -1, 10), (100, 1, 11), 3),
            ((-1, -1, -2), (1, 1, 20), 3),
            ((-1, -1, -3), (1, 1, -2), 0),
            ((50, -1, 10), (100, 1, 20), 0),
        ):
            self.assertEqual(self.native.screen(v(*lo), v(*hi), p, 1, 0, 1), verdict)
        self.assertEqual(self.native.screen(v(-1, -1, 10), v(100, 1, 11), p, 1, 64, 1), 2)
        self.assertEqual(self.native.screen(v(-1, -1, 10), v(100, 100, 11), p, 1, 64, 1), 3)
        self.assertEqual(self.native.screen(v(-1, -1, -2), v(100, 100, 11), p, 1, 64, 1), 2)
        # ICO near rule: a box reaching behind the near plane stays a whole call unless it passes the guard window
        self.assertEqual(self.native.screen(v(-1, -1, -2), v(1, 1, 20), p, 1, 0, 0), 1)
        self.assertEqual(self.native.screen(v(-1, -1, -3), v(1, 1, -2), p, 1, 0, 0), 0)
        self.assertEqual(self.native.screen(v(-1, -1, -2), v(100, 1, 11), p, 1, 0, 0), 3)

    def test_world_tessellates_only_selected_interior_edges_within_budget(self):
        points = [(x * 10, y * 10, 0) for x in range(8) for y in (0, 1)]
        g = geometry(points, [tuple(range(len(points)))])
        g['strip'] = 1
        blob, stats = make_entry(g, 0, None, world=True, split=(0,), budget=2)
        self.assertEqual(self.native.validate(blob, len(blob)), 1)
        self.assertEqual(stats['output_triangles'] - stats['input_triangles'], 2)
        self.assertEqual(stats['vertices'], len(points) + 1)
        self.assertEqual(HEADER.unpack_from(blob)[1], len(points))

    def test_unsafe_inputs_have_no_float_fallback(self):
        for points in ([(0,0,0),(.01,0,0),(.01,0,.01),(0,0,.01)],
                       [(0,0,0),(100000,0,0),(100000,0,100000),(0,0,100000)]):
            with self.assertRaises(ValueError):
                make_entry(geometry(points), 0, self.optimizer)


if __name__ == '__main__':
    unittest.main()
