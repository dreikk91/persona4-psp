# b451: offline static geometry prototype

The preparation utility now builds `p4static.bin` from the player's ISO, and the renderer can consume its compact geometry and GE command templates. This is a per-geometry compiler, not a complete scene compiler. The runtime path defaults off (`p4_static_pack=0`) pending physical PSP validation.

## Representation and optimization

- Pinned meshoptimizer v0.25 performs attribute-aware, border-locked simplification, index ordering and verified stripification. UVs, normals, colours, material boundaries and coincident attribute seams participate in preservation. Exact duplicate attributes can merge. Degenerate source index triangles are removed.
- Positions use s8 when the common grid fits, s16 otherwise. Normals use s8, UVs and indices use u16, colours RGBA8888. There are no float vertex streams or float vertex fallback inside the pack. Floating-point values remain in matrix/UV decode metadata.
- Positions snap to one absolute 0.25-unit model-space lattice before subtracting a grid-aligned centre. Thus the same source coordinate decodes identically with different centres and s8/s16 encodings, including half-step ties. This does not guarantee identical world-space seams for independently transformed objects.
- The simplification error cap is 0.125 model units, with UV/normal/colour weights. Position quantization has a separate maximum of sqrt(3) * 0.125 units. These are model-space limits, not a universal screen-space quality guarantee.
- GE templates contain only validated VTYPE, address, BOUNDINGBOX/BJUMP, PRIM and RET commands. Runtime relocation patches pointers; textures, lighting, materials, model matrices and both GS alpha-test passes use the existing renderer. Bounding boxes conservatively enclose the geometry.
- Pack records load lazily at first eligible draw into the existing static-list budget. The index stays resident. Entries retire with the existing delayed-free mechanism; this does not make gameplay allocation-free.

Unsupported or changed geometry uses the original renderer: skin, morph, MatFX, non-opaque texels/prelight, tinted unlit materials, extreme UV ranges, integer overflow, quantization that collapses/flips a triangle, or camera-dependent guard clipping. That original path can still use floats. No current Persona/enemy skin reduction is introduced by this prototype; their existing offline strip pack remains separate.

## Recorded UNDUB generation

The tested USA UNDUB executable is the same SHA-1 accepted by the preparation utility. Generated game data stays local.

| Result | Count |
|---|---:|
| Pack variants (lit/unlit counted separately) | 18,426 |
| s8 variants | 6,437 |
| s16 variants | 11,989 |
| Pack bytes | 56,559,128 |
| Triangle count across retained variants, before | 2,024,019 |
| Triangle count across retained variants, after | 2,018,011 |

This is about 0.30% triangle reduction under the conservative attribute/seam limits, not a full-game polygon or FPS reduction. The main implemented change is preparing compact streams, topology and command templates on the PC. The largest measured position error was 0.216102 units and the largest reported simplification error 0.124795 units. Entries that violate the bounds are excluded and listed in the adjacent generation JSON.

## Validation

- Six host tests exercise integer formats, s8/s16 seam agreement around different centres, locked-border simplification, winding, invalid command/address rejection, truncated packets, relocation across a BASE boundary, source hashes and unsafe-input rejection.
- Source-build/cache/install tests: 16 passed, including all three installed packs and rejection of a changed host optimizer DLL. Preparation tests: 6 passed. Existing UI draw test: 16,384 configurations passed.
- Diagnostic PSP build linked and validated 19 import libraries with zero unresolved function-pointer targets. Its sources were hashed against the current tree.
- The actual frozen executable generated a sample pack through its build worker, then completed a clean UNDUB source build and build-only installation. All 1416 recovered C files compiled; EBOOT, data, strings, strips, CLUT and static pack passed final SHA256 checks. The installed static pack is byte-identical to standalone generation.
- A separate PPSSPP process fresh-booted diagnostic b451 and loaded a copied native slot-3 save: Heaven, 10th Paradise, field 28 room 2, step 110. No savestate was used. Game archives/media came from the existing USA installation; the UNDUB-derived geometry pack matches by full content identity rather than disc offsets.
- With the new path and source checking enabled, one recorded window reported 2,802 pack draws (560 s8, 2,242 s16), 61 source checks, zero malformed entries and zero source differences. Later stationary windows reached 10,240 draws and 244 checks per window, still zero differences. A short forward walk exercised the moving-camera path and fallback without a new exception in the observed route.
- Native-resolution display off/on/off captures were inspected. No obvious geometry holes were seen in the inspected frames; pixel equivalence is not established. In the statue/stair rectangle (105,20)-(370,150), 6,447 of 34,450 pixels differed off/on, 344 by more than 3 channel levels and 89 by more than 16; mean absolute channel difference was 0.24546. Off/off differed at 4 pixels. Quantization/interpolation differences are real; the character region also changes with idle animation.
- Existing startup JIT diagnostics at address zero remain. Neither physical PSP performance, a full playthrough, full media conversion nor broad near-camera/scene coverage was tested. The generated XMB release EBOOT needs separate gameplay validation.

Evidence is retained locally under `build/static_b451/` (validation JSON, runtime log and display captures), `build/static_b451_final/` (pack/report), and the verified source-build cache. The local source utility is `build/static_b451/P4-PSP-b451-local/`; it does not include generated game files.

## Reproduction

Use a configured PSPDEV and the normal build prerequisites. Generate all packs in one folder before passing it to the diagnostic linker:

```powershell
python tools/psp_strips.py --iso './Persona 4 (UNDUB).iso' --static --out ./build/static/p4strips.bin
python tools/psp_clut.py --iso './Persona 4 (UNDUB).iso' --out ./build/static/p4clut.bin
python tools/psp_static_pack.py --iso './Persona 4 (UNDUB).iso' --out ./build/static/p4static.bin
python tools/psp_hw_debug.py --asset-packs ./build/static --out ./build/psplink_b451
```

Install matching diagnostic launch files and all packs beside the game's archives; use a copied native save. Enable `p4_static_pack=1` before entering the test scene. `p4_static_pack_check=1` periodically rehashes source arrays and verifies the binding contract; it does not compare pixels or simplified geometry. Resolve both symbol addresses from the exact deployed ELF for every rebuild. Restore both controls to zero after the experiment. Measure off/on/off on a physical PSP at 333 MHz with checking/profiling off before making a speed claim.

## World batch experiment

The optional world pack uses the same RenderWare geometry identity and `p4static.bin` index. It stores s16 positions, u16 UVs, s8 normals where present, a shared vertex stream, and model-space AABBs. Spatial cuts follow jumps between consecutive triangle centres so the clouds retain their material and triangle order. Each batch has at most 64 triangles. The packer keeps ordered strips when they reduce the index count.

Generate the Heaven pack from the USA ISO:

```powershell
python tools/psp_static_pack.py --world --path '/FIELD/PACK/F028_' --out build/world_heaven/p4static.bin
```

For the comparison with targeted subdivision and the Heaven boss arena from `F244_003`:

```powershell
python tools/psp_static_pack.py --world --path '/FIELD/PACK/F028_' --path '/FIELD/PACK/F244_003.P01' --split-texture f028_002_cloud --split-texture f244_wall_deco02_b --out build/world_heaven_split/p4static.bin
```

Subdivision is optional and matches exact texture names. It bisects paired interior edges only, leaves material seams and boundary edges alone, and caps added triangles at 20% per material and geometry and 1,024 per pack. The selected textures appear in prior guard-cut logs. The runtime still needs bounded clipping for batches crossing the GE window or the near plane.

| Pack | Bytes | Triangles before → after | Batches | Offline GE words |
|---|---:|---:|---:|---:|
| Heaven | 911,936 | 36,718 → 36,718 | 862 | 5,442 |
| Heaven + boss arena, selected subdivision | 1,067,984 | 41,214 → 41,678 | 968 | 6,102 |

Pack totals include lit and unlit variants. The renderer tests the whole AABB before setting draw state. Fully visible geometry calls one prepared list per material and alpha pass. At the screen edge it tests the batch AABBs against the existing GE guard window and builds one visible list per material, clipping only batches that cross that window or the near plane. The static world log reports whole-list calls, culled geometries, exceptional batches and template words per 256 frames. Geometry that fails binding or eligibility checks falls back to the existing draw path.

Use the copied native slot-3 save in Heaven, 10th Paradise. Compare the same camera with the world pack enabled, with the pack disabled and guard on, and with both the pack and guard disabled. With `p4_static_pack=1` and `p4_guard_clip=0`, the world pack still clips exceptional batches and unpacked geometry uses the default GE path. Use `p4_static_pack_check=1` for binding verification and turn it off for timings. For the arena use the `F244_003` boss encounter. Physical PSP is required to verify guard-window artifacts and CPU timings.

PPSSPP bridge comparison with `p4_guard_clip=0` throughout: pack on → off → on. The bright horizontal cloud bands appeared with the pack off and disappeared when it was re-enabled. One 256-frame bridge window recorded zero legacy guard cuts, 11,264 world clip batches, 726,272 GE template words and 308 source checks with zero differences. The capture is retained locally at `build/world_heaven/bridge_guard0_comparison.png` with the live log in `build/world_heaven/independent-clip-runtime.log`. This checks the cloud view in PPSSPP. Arena visuals and physical PSP timings are still pending.

For the ICO auto comparison use `p4_static_pack=2` alongside `p4_guard_clip=0`. Mode 2 uses a 1,024-pixel margin and requires at least 16 screens of projected batch AABB area before invoking the CPU clipper. A batch with a box crossing the eye is left to the GE. Mode 1 uses the P4 GE window and near-plane criteria. The legacy guard switch is independent in both modes.

The same-camera PPSSPP bridge comparison with guard off used pack modes 1 → 2 → 0 → 1. The bright horizontal cloud bands appeared with mode 2 and with the pack off, and disappeared in both mode-1 captures. See `build/world_heaven/ico_comparison.png` and `build/world_heaven/ico-runtime.log`. Mode 2 remains an experimental control and defaults off.
