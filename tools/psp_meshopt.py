"""Pinned meshoptimizer host library; never linked into the PSP executable."""
import ctypes as C
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def build_library():
    source = ROOT / 'third_party/meshoptimizer/src'
    output = ROOT / 'build/p4meshopt.dll'
    output.parent.mkdir(exist_ok=True)
    files = [source / (n + '.cpp') for n in
             ('allocator', 'indexgenerator', 'vcacheoptimizer', 'vfetchoptimizer', 'stripifier', 'simplifier')]
    compiler = shutil.which('g++')
    if not compiler:
        raise RuntimeError('Packaging needs a host g++ compiler; the frozen utility includes the compiled helper.')
    if not output.exists() or any(p.stat().st_mtime > output.stat().st_mtime for p in [*files, source / 'meshoptimizer.h']):
        subprocess.run([compiler, '-std=c++11', '-O2', '-shared', '-static',
                        '-DMESHOPTIMIZER_API=__declspec(dllexport)', *map(str, files), '-o', str(output)], check=True)
    return output


class Meshopt:
    def __init__(self, library=None):
        bundled = Path(sys.executable).parent / 'helpers/p4meshopt.dll'
        path = library or os.environ.get('P4_MESHOPT') or (bundled if bundled.is_file() else build_library())
        self.lib = C.CDLL(str(Path(path).resolve()))
        u, f, b, z = C.POINTER(C.c_uint), C.POINTER(C.c_float), C.POINTER(C.c_ubyte), C.c_size_t
        self.lib.meshopt_optimizeVertexCacheStrip.argtypes = [u, u, z, z]
        self.lib.meshopt_stripifyBound.argtypes = [z]
        self.lib.meshopt_stripifyBound.restype = z
        self.lib.meshopt_stripify.argtypes = [u, u, z, z, C.c_uint]
        self.lib.meshopt_stripify.restype = z
        self.lib.meshopt_simplifyWithAttributes.argtypes = [u, u, z, f, z, z, f, z, f, z, b, z, C.c_float, C.c_uint, f]
        self.lib.meshopt_simplifyWithAttributes.restype = z

    def optimize(self, triangles, positions, attributes, locks, error):
        indices = [v for t in triangles for v in t]
        nv, na = len(positions), len(attributes[0])
        source = (C.c_uint * len(indices))(*indices)
        out = (C.c_uint * len(indices))()
        achieved = C.c_float()
        if error >= 0 and len(indices) >= 12:
            pos = (C.c_float * (nv * 3))(*(v for p in positions for v in p))
            attr = (C.c_float * (nv * na))(*(v for a in attributes for v in a))
            weights = (C.c_float * na)(512, 512, 4, 4, 4, 255, 255, 255, 255)
            locked = (C.c_ubyte * nv)(*locks)
            count = self.lib.meshopt_simplifyWithAttributes(out, source, len(source), pos, nv, 12,
                        attr, na * 4, weights, na, locked, 3, error, 1 | 2 | 4, C.byref(achieved))
            if count >= 3:
                indices = list(out[:count])
                source = (C.c_uint * count)(*indices)
                out = (C.c_uint * count)()
        self.lib.meshopt_optimizeVertexCacheStrip(out, source, len(source), nv)
        count = self.lib.meshopt_stripifyBound(len(source))
        strips = (C.c_uint * count)()
        count = self.lib.meshopt_stripify(strips, out, len(source), nv, 0)
        return [tuple(out[i:i + 3]) for i in range(0, len(source), 3)], list(strips[:count]), achieved.value
