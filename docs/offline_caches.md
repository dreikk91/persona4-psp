# Port caches: offline data with the resource's lifetime (plan, 2026-11-19)

Goal: during play (between loads) the port allocates nothing from the game's heap. What the PSP needs in its own
format is made on the PC and loaded with the resource it belongs to, and freed with it, as the PS2 game's own data
is. The few buffers that stay runtime-made are fixed pools taken once at boot.

## Why

b412 on the PSP, after Heaven (battle, events) going to floor step 109 (`f044_000` with the party models): a 2 MB
request failed with 19 MB free and the largest run 832 KB, the game stopped (log: `heap short: request 2048112`, heap
map after). The same failure as b373-b375. The cause is when the caches allocate, not how much: a cache block is made
after its model, at a moment of its own, between the game's blocks; it outlives or predeceases the model and leaves
holes. b376 put the caches in their own 12 MB arena; b378 turned it off (`p4_arena_kb 0`): the arena filled, the
static lists re-recorded every frame and a frame took 106 ms.

The PS2 had no such caches: VU1 converted the game's data while drawing. The GE cannot read those formats, so the
port converts on the PSP and keeps the result. Converting on the PC instead also removes the build hitches.

## What the caches held before the b412 failure

| Cache | Bytes | Offline | Notes |
|---|---|---|---|
| Skin GE batches (`skin_geometry`, skin.h) | 3.38 MB | yes | strips already from the PC (`p4strips.bin`, tools/psp_strips.py); keyed by geometry, LRU of 64 |
| Swizzled textures + mip levels (`swizzle_raster`, rwsky.c) | 6.70 + 2.12 MB | yes | replaces the original pixels; for a moment both exist, the original's hole stays |
| Static lists (static_list.h) | 2.24 MB | in part | vertex data offline; the command words depend on state at draw time |
| Mesh cache | 1.20 MB (budget 6 MB) | yes | the same vertex data as the static lists |
| Far stand-ins (far_lod.h) | 0.32 MB | yes | can be baked |
| GE call ring, particle spill, topology, guard results | 1 + 2 + 0.2 + 0.02 MB | no | runtime by nature: fixed pools at boot |

## How

- Side files keyed by a hash of the resource, as `p4strips.bin` is (the game's own RenderWare files stay as they
  are: the game parses them). Read per resource from the memory stick at load, not kept resident (`p4strips.bin`
  alone is 7.2 MB).
- Lifetime: allocate when the game loads the resource, free when it destroys it (hooks on the RenderWare
  create/destroy calls, by address as the other overrides). No LRU, no eviction during play.
- Every offline product has a `_check` mode that builds the PSP's own result and compares, as the existing switches
  do. A resource with no offline entry takes today's runtime path, so missing data costs time, not correctness.
- The offline converter must match the runtime conversion bit for bit for the switches that are on (skin pack,
  wsplit 2, nocolour, flat, palette 8; static flat, UV/position pack, colour16).

## Order

1. Skin GE batches: largest gain for the least new work (the batch cut and the strips are already on the PC).
   Variants by `lit`/`nrm` and `nbones` need settling first (what decides them at load time).
2. Textures: pre-swizzled pixels and mip levels, loaded in place of the originals.
3. Static meshes: vertex data for the static lists and the mesh cache; the lists keep recording their commands.
4. Fixed pools at boot for what stays runtime; then the arena (b376) again for any remainder, smaller and with
   per-cache budgets, and a refused block retried only after N frames (the b376 failure was the retry every frame).

Measure on the PSP after each step: heap map after a long session (Heaven, a battle, a floor change), largest free
run, load time, CPU in the reference scenes.

## Optional strip and CLUT packs for PSP testing

Generate both packs from the local ISO and put them next to the diagnostic EBOOT/PRX:

```text
python tools/psp_strips.py --static --out build/offline-ab/p4strips.bin
python tools/psp_clut.py --out build/offline-ab/p4clut.bin
python tools/psp_hw_debug.py --asset-packs build/offline-ab --out build/offline-ab/diagnostic
```

- `p4_mesh_strip_pack=1` uses shorter, verified joined strips for eligible static mesh caches and looks up
  strips for skin weight groups (`p4_strip_pack=1`). Missing groups continue to build strips on the PSP.
  Check `offline strips` for actual hits. Static meshes without a smaller joined strip, uncached geometry,
  and the separate clipping path keep their normal draw path.
- `p4_clut_pack=1` with `p4_clut32=1` uses precomputed palettes and indices for matching native RGBA32 textures.
  Exact textures can use CLUT4 or CLUT8. More than 256 colours uses lossy CLUT8 (Pillow is required to build
  the pack). Check `clut pack hit` and compare alpha edges, shadows and UI. The PSP still swizzles the copy
  and builds mip levels, and keeps the game's original raster for normal lock/write and fallback behaviour.
- Both new switches default to `0`. Set them before entering the test scene in a fresh run, then repeat from
  the same native save with one switch changed. Existing cached resources keep the mode they were built with.
  Measure first and repeated summons separately, and correlate the long frame with `SKINC`, `clut32 made`,
  `clut pack hit` and the model read. Test the two switches separately before testing both together.
