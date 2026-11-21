# PSP performance through b450

Hardware observations below are from the named diagnostic builds at 333 MHz. The b450 XMB release needs independent hardware validation. See [the changelog](../CHANGELOG.md) for retained changes and [release validation](build_validation_b450.md).

## b432-b447 results (PSP, 333 MHz, 2026-11-20/21)

Reference run: Heaven, the user's recorded route (up 6.26 s, down 6.33 s, up 5.27 s, down 5.53 s; replayed by
pad pokes after a scene-launcher reload of floor 110). CPU ms per 128-frame window; profiled numbers only against
profiled ones.

**Kept (on by default):**
- `p4_ge_compile=1` (b436-b438, default from b445): every flattened static list is made GE-native once: s16 or s8
  positions around the list's box centre (decode folded into the model matrix), triangle lists as strips with PRIM
  auto-advance (stripifier host-tested on 200 random meshes: same triangles and winding, ~45% fewer vertices),
  BOUNDINGBOX/BJUMP prefix, uniform-colour lists without vertex colour (AMC/AMA set after the box test). Heaven:
  list data -40% (2.65 -> 1.62 MB), picture unchanged, CPU unchanged (the frame is CPU-bound).
- b439: a static list is keyed by the generations of the rasters it binds, not the global `texture_generation`.
  Before, every typed dialogue letter (font raster) re-recorded the whole room: 445 ms frames (Margaret before
  battle).
- b446: the per-atomic hot path in one code section (`P4_HOT`, `.text.p4hot`): `atomic` imiss 17.6k -> 15.6k,
  -0.4 ms; frame-wide misses unchanged (~39k imiss, ~29k dmiss a frame; ~56% of a profiled 37 ms frame is
  memory stall). The frame's code is larger than the 16 KB icache: capacity misses, not layout.

**Measured, left off (switches default 0):**
- The CPU guard costs ~9 ms CPU running (b441 profile: `atomic` +6.7, `asl` +2.4; the cut itself `gclip` 0.4 ms;
  frameskip adds ~4-5 ms more). `p4_guard_clip=0` gives ~42 vs ~51 ms but the cloud sea under Heaven smears
  (big triangles behind the eye) and the battle arena floor gets holes.
- `p4_ge_split` (+`_min`, `_grow`; b434-b440): subdivide long edges at cache build. All meshes at 1024: CPU back to
  base (bus: lists 1.0 -> 3.6 MB); clouds only (min 3800) at 1024: right picture, ~48 ms (-4 vs base, peaks 51 vs
  62) but in battle the arena/backdrop grew 8.4k -> 164k triangles (7.5 FPS, list budget full, Persona summon ran
  out of heap). Growth cap 32 contains it but leaves the big meshes on the guard.
- `p4_guard_tiny` 1100 with split clouds: guard on everything except small/split geometry (no arena holes);
  untested for speed after the battle problem.
- `p4_static_chunk` (b441): parts of N triangles with own lists: 128 ~56 ms, 64 ~64 ms (each crossing part pays
  the whole long path).
- `p4_static_gate` (b442-b447): one uncut list per crossing atomic, a NOP/JUMP gate per 32-triangle chunk, three
  copies (reuse only after `ge_finished`), closed chunks cut from kept copies (lit meshes from b447). Works,
  picture right, no gain: only ~5 of ~20 guarded meshes a frame come through it (mesh-split atomics and other
  paths do not), and `asl` +3.5 ms.
- `p4_guard_snap` (b444): the guard planes for the camera translation rounded to a grid: guard results still miss
  (the follow camera rotates every frame).
- `p4_static_guard_lists=0`: no change (guard lists are not reused while running anyway).
- `p4_2d_cache=1` in Heaven: within noise (little 2D there; retry in a dialogue).
- b429 GE controls (Margaret, b433): `p4_state_call` ~66 hits/frame but CPU +0.45 ms; `p4_chain_strips` 0 chains
  (`gu_dirty` breaks every 2D run); `p4_compact_immediate` 0 vertices (UI coordinates are fractional after
  640x448 -> 480x272); `p4_clear_stripes=0` -0.15 ms CPU.
- `p4_mesh_strip_pack`, `p4_clut_pack` (offline packs): Heaven coverage tiny (22 static hits, 1 texture), no change.
- `p4_strip_prims=1` (b250 retest): coverage confirmed, no change, 715 ms rebuild hitch when switched.

Open: the Media Engine hang on movies happened twice (intro `mpeg delete`, title return `sceMpegInit`, both
`user_main` in `SceMediaEngineRpcWait`): `build/psplink_b433/*hang.log`.

Next candidates: deferred drawing of opaque static lists (keep the renderer hot in the icache instead of
interleaving with game code per atomic), animation (`qmodel`/`aadv` ~9 ms) on the Media Engine.
