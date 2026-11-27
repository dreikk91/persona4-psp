# PSP performance: current status and plan (2026-11-15, build 267)

The current state of the PSP performance work. The full history of every build (b97-b267: what was tried, the
measurements, why things are on or off) is in [archive/perf_history.md](archive/perf_history.md); search it by
switch name or build number before retrying an idea.

Goal from the user: a stable 30 FPS on a real PSP at **333 MHz**, with effects adapted rather than removed.
Distance-based simplification (LOD, stand-ins) is fine where the PSP screen cannot show the detail. Numbers are from
a real PSP at 333 MHz. Never compare profiled (`profmode t`, ~10 ms CPU a frame) with unprofiled numbers. The
b170-b176 numbers (2026-11-10) were taken at 423 MHz by mistake.

## Where the frame is

Frameskip draws one tick and skips the next; `cpu_us` is wall time from present to sync and holds the skipped tick
too. 20 FPS (a 50 ms frame) needs `cpu_us` under ~48 ms, 30 FPS under ~33 ms.

| Scene | Frame | CPU (`cpu_us`) | Notes |
|---|---|---|---|
| Heaven, 10th Paradise | 66.7 ms (15 FPS) | ~55.6 ms (b263) | static lists -4.3 ms, stand-ins ~-2 ms; GE wait ~10 ms |
| Margaret dialogue | 67 ms | ~52 ms (b263) | GE ~31 ms a frame: atomics 22.9, 2D 8.4 |
| Battle, held menu | 50 ms (20 FPS) | 41-46 ms (b264-b265) | arena per-mesh lists -0.5..-1.4 ms |
| Shuffle Time help overlay | ~188 ms (5 FPS) | ~188 ms (b265) | `btlShuffleEffStep` ~89%; see [shuffle_time_perf.md](shuffle_time_perf.md) |

Dialogue, b263 profiled, cycles a frame (inclusive, do not add up): frame 18.1 M; `qmodel` 4.4 M, `alayer` 3.2 M,
`kadd` 1.5 M, `kmat` 1.2 M, `actrl` 0.85 M, `kinterp` 0.72 M, `apose` 0.6 M; `geskin` 1.5 M, `mesh` 1.6 M, 2D 1.0 M;
skipped tick `SKIP mainDraw` 1.4 M, `SKIP linkOt` 0.8 M. Animation is the largest reserve:
[animation_psp.md](animation_psp.md).

## What is on

The render paths that carry the numbers above (all with a switch for A/B; most have a `_check` mode that compares
against the old path):

- **Static scenery as prepared lists** (`p4_static_list`, b258, [static_list.h](../port/static_list.h)): one recorded
  CALL per static atomic; arena atomics that cross the guard band split per mesh (`p4_static_mesh_lists`, b264,
  [arena_b264.md](arena_b264.md)).
- **Skinned characters**: GE skinning with strips packed on the PC (`p4_ge_skin`, `p4_skin_strip`, `p4_strip_pack`),
  8-bone palettes, one recorded list per character (`p4_skin_list`, b260), bone LOD by distance (`p4_skin_lod_depth`
  900, far level `p4_skin_far_depth` 3000 with 8 bones, [bone_lod.h](../port/bone_lod.h)).
- **Far objects** ([far_lod.h](../port/far_lod.h)): instance batching of small repeated statics (`p4_inst_batch`),
  untextured baked stand-ins beyond `p4_far_proxy` 300 (b257/b262). Far culling is off (`p4_cull_cover` 0,
  `p4_cull_far` 0): far models stay drawn.
- **Per-mesh and per-batch CALL replays** (`p4_mesh_call`, `p4_skin_call`, b189-b190), the mesh and topology caches,
  cached guard-band results (`p4_guard_result`, b236).
- **Animation**: C/VFPU hierarchy update (`p4_hanim_c`, `p4_hanim_vfpu`), lazy and far-step animation
  (`p4_anim_lazy`, `p4_anim_far`), native key decode (`p4_anim_native`, `p4_anim_keymap`).
- **Shadows**: capsule shadows and the look-at shadow pose (`p4_shadow_capsule`, `p4_shadow_lookat`).
- **Particles**: VFPU path for the particle matrix block (`p4_particle_vfpu`, b266, device check 0 diffs); the
  particle vertex pool reserves clipped quads instead of refusing whole tanks.
- **GE words written directly** (`p4_ge_raw`, b365-b366): state cache, texture binds, model/view matrices, light,
  blend, stencil, fog and every `sceGuDrawArray` (batched stall updates, no syscall per draw) without libpspgu.
  Same words; PSP A/B pending. The dialogue's 67 ms is the CPU and the GE not overlapping, not CPU work alone:
  [ge_frame.md](ge_frame.md) (b360 PSP numbers, `p4_timeline`, double-buffered lists `p4_list_double`, why skins
  carry 8 weights a vertex).

## Measured and left off

Each was measured on the PSP and gave no gain (details under its build in the history):
`p4_mesh_persist` (b245), `p4_interp_fast`/`p4_interp_batch`/`p4_interp_pairs` (b234-b235), `p4_pose_runs`
(b237-b238), `p4_guard_split` (b232-b233), `p4_2d_batch` (b261), `p4_skin_rigid` and palette sizes other than 8
(b207, b262), `p4_rwmul_vfpu` (b230), `p4_ge_static_clip` (b267), `p4_skin_simplify` (b211, tears hair online).
Not measured, still a candidate: `p4_anim_cull` (no animation for models not drawn, b173). Also off: `p4_anim_tracks` (b272, exact, no gain). Lossy culling (`p4_cull_far`, `p4_cull_cover`) is off by the user's choice.

## Plan, in order

1. **Code cleanup (2026-11-15, in progress).** Done in b268 (not yet run on the PSP): the experiments above that
   were measured dead removed with their switches. Next: split `port/rwsky.c` (~9.8k lines) into modules without
   changing behaviour, checking on the PSP after
   each step. Reasons: `.bss` keeps running into the fixed `.p4image` section (five times in b256-b263, fixed by
   moving arrays to the heap), and the hot paths miss the instruction cache (~146 misses a `mesh` call in b257).
2. **Lighter animation** ([animation_psp.md](animation_psp.md)): per-node key tracks (`p4_anim_tracks`, b269-b272)
   are exact on the PSP but gain nothing in the dialogue (cpu 56.6 / 56.6 / 56.4 / 56.3 ms off/on/off/on): off.
   Dialogue animation by size: `kmat` 0.96 M, `kadd` 0.94 M, `actrl` 0.76 M, `apose` 0.56 M, `kset` 0.32 M.
   Candidates: animation update rate for background characters (a quality trade: user's call), `actrl` breakdown,
   a one-pass skeleton update.
3. **Skipped tick**: `SKIP mainDraw` and `SKIP linkOt` (0.8-2.5 M cycles) run drawing work in a tick that draws
   nothing; b198 kept the OT callbacks for their side effects. Find which callbacks really need it.
4. **Shuffle Time**: keep effect clocks/spawn/lifetime in skipped ticks, move pure particle projection to drawn
   ticks ([shuffle_time_perf.md](shuffle_time_perf.md)).
5. Optional: split skin batches by weight count (GE), CLUT8 for 32-bit backgrounds, a breakdown of the 2D GE time.
6. **Idea (2026-11-25, not started): use the volatile 4 MB during gameplay.** The block at 0x08400000
   (`sceKernelVolatileMemTryLock`) is locked only while a movie plays (`video.c`) and for a b497 screenshot
   (`rwsky.c` p4_shot, a second); the rest of the time it is idle. Another port on a PSP-3000 put GE display
   lists and texture arenas there: +4 MB for the game heap, 3.8 ms a frame saved through a bigger streaming budget
   (psp-developer skill, memory-map.md section 6). For P4: hand it back before every movie (the movie task already
   drains the GE and releases caches first), hold `scePowerLock(0)` while it is ours, release it before system
   dialogs (saves). Candidates: the two frame lists (`list_store`), the vertex/index pools, the skin/static strip
   caches. Addresses below 0x0A000000 also stay readable by PSPLink `savemem`.

Open items from older plans that were not re-checked (rendering gaps from 2026-11-09, movie or SAS hang after
~10 min of sound): [archive/next_steps_b112.md](archive/next_steps_b112.md), [audio_video_hang.md](audio_video_hang.md).

## Handoff for the next session (2026-11-21, b449)

**Goal (user):** stable 30 FPS on the PSP at 333 MHz, effects adapted, not removed. The user wants PSP GE best
practice as rules, not experiments: strips with PRIM auto-advance, s8/s16 positions (scale/offset in the world
matrix), s8 normals, u16/u8 UVs, 16-bit or no vertex colour, BOUNDINGBOX/BJUMP on prepared lists, no doubles, CLUT
textures, 64-px clears. Keep tests short (one route pass, 2-3 windows an arm); no new counters unless they decide
the next step.

**Where the frame goes** (b447 profiled, Heaven standing; disabling one part at a time, a base between arms):
frame 44.5 ms, of it **24.8 ms (56%) memory stall** (~50k instruction, ~42k data cache misses a frame). Without all
3D -23.6 ms (-15 ms stall, -27k imiss); without 2D -8.9 ms (-14k imiss: 2D code evicts everything else); animation
frozen -4.7 ms; particles 0; only game logic left: 11 ms. The cost is CPU cache misses, not arithmetic, not GE.

**State of the tree:** branch `perf-b429-b447` (commit `6115192`), plus b448-b449 on top (2D, below). PSP runs b449
(`profmode t` set: restart with `profmode t` + `reset` + `ldstart`; after a manual PSPLink restart `reset` may hang).
Defaults now: `p4_ge_compile=1`, `p4_2d_cache=1`, `p4_2d_texonly=1` (no effect so far), raster-keyed lists (b439),
`P4_HOT` section (b446). All guard experiments default 0.

**Next, in order:**
1. **2D state (dialogue: 157 draws/frame, 2D 4.9 ms, state 2.4 ms).** `p4_2d_cache` keeps ~55 draws' state, ~100 set
   it again. b448's texture-only path never hit (`texture only 0`). b449 logs why (`2d cache ... set because: not
   plain / epoch / target / alpha / mask / texgen / sky / render (rs index)`): read it in a dialogue first, then
   build the thin per-sprite path for the real cause, P3P style (P3P shares P4's UI code: frFont, frFontRaster,
   itfMesManager, bustupDraw, dtDraw; it draws each sprite as a 4-vertex strip, `0x80019e`, 20-byte vertices with
   u16 texel UVs, through a thin SDK wrapper: `ext/P3P/ghidra/out/decomp.c`, `FUN_002a4aac`, `FUN_002a2c40`).
2. **Bones as one flat array** (animation + skin bone preparation, ~7 ms in Heaven, more in dialogue/battle).
   Ceiling measured (`p4_anim_freeze`, profiled, Heaven standing): layer advance -2.2 ms, bone matrices/frames -3.2,
   control 0, all -5.3. Skin prep `geskin` ~450 us a character, mostly reading scattered `RwFrame`s. P3P's model:
   flat bone array, dirty bones only, layers blended in place, frames written only for the ~35 read of ~640
   ([animation_psp.md](animation_psp.md#L168)). A whole engine swap needs the game's interpolator fields kept
   consistent: switch + check mode, offline compact animation pack later.
3. **Deferred drawing of opaque static lists:** collect in the atomic callback, draw in one loop at the end of the
   frame so the renderer stays in the icache (code layout alone, b446, gave only -0.4 ms).
4. Guard (~9 ms running): parked; see the results below before trying again. reVC's spatial 8x8 triangle cells could
   shorten `gloop`+`gsub` (~2 ms) once all crossing objects share one path.
5. Open: Media Engine hangs on movies (`build/psplink_b433/*hang.log`).

**Course (user decision, 2026-11-21): move off RenderWare in the hot paths.** RenderWare stays only as the contract
the game code uses (its function names and the struct fields it reads and writes); behind it a thin PSP-native
engine modelled on P3P's AwSDK (flat arrays, VFPU, GE-ready data, a few words a draw). AwSDK itself cannot be
linked in: P4's compiled game code calls RW and reads RW structs by offset; switching APIs would mean recompiling the
game. Module by module, each behind a switch with a check mode and a short measurement:
1. **2D:** sprites and glyphs emit their GE words themselves, no `RwRenderStateSet`/Im2D on the way (P3P draws a
   sprite as one 4-vertex strip, `0x80019e`, 20-byte vertices: `FUN_002a2c40`, `FUN_002a4aac`).
2. **Animation and bones:** one flat bone array, layers blended in place, RW frames written only for bones that are
   read (P3P model runtime, [animation_psp.md](animation_psp.md#L168)); an offline compact animation pack later.
3. **World geometry as an offline pack, GMO-style** (own format, not GMO itself: it also holds our prepared CALL
   lists, patch points and seam-safe s16/s8 grids), keyed by RW geometry like `p4strips.bin`: GE-ready strips,
   BOUNDINGBOX/BJUMP per mesh, and grids or GE Bezier/spline patches (`PATCHDIVISION`) instead of PS2-sized
   triangles, so the world needs no CPU guard. P3P's GMO path (`FUN_003503f8` -> `FUN_003558b0` arrays, `FUN_00355b30`
   rect grid as strips, `FUN_00356be0`/`FUN_00356ca0` BEZIER/SPLINE, `FUN_003553a0` box) has no CPU clipping at all.
   First experiment: Heaven's cloud planes as GE Bezier patches (control points only on the bus, GE tessellates).
   Dynamic parts (UV animation, morph, material colour, MatFX env) need patch points or the RW path.
4. **Per-frame object walk:** deferred drawing instead of RW's per-atomic callback in the middle of game logic.

Already replaced: platform (rasters, GE, Im3D), atomic/geometry rendering (mesh cache, static/skin lists, compile),
part of Im2D/render state (b336, `p4_2d_lean`, `p4_2d_cache`), HAnim on the VFPU, lazy frames, animation tracks.

Reference to watch: a P3P mod (2026-11, PC, author codes with Claude; repo announced) converts FES's RenderWare field
maps into P3P's 3D field format and runs them on P3P's own field engine; its converter will show P3P's map format
and how it handled PS2-sized triangles. No PSP hardware or FPS data yet.

**How the short tests were run** (scripts in `${P4_SCRATCH}` on this PC, not in the repo): reload Heaven with
`tools/psp_scene.py floor 110`, wait for 2-3 steady `hwperf` windows, replay the user's route by pad pokes
(`p4_pad_press`/`p4_pad_reads`: hold UP 6.26 s, wait 1.0, DOWN 6.33, wait 1.2, UP 5.27, wait 0.22, DOWN 5.53) while
`tools/psp_probe_ab.py LABEL --samples 3` measures; screenshot by `scrshot host0:/bNNN/shot_LABEL.bmp` in the middle
of the first UP. Toggle switches by `pokew` (symbol + 0x08804000) before the reload. Never edit sources on the share
while a probe runs: it refuses a build whose sources changed.

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
`user_main` in `SceMediaEngineRpcWait`): `build/psplink_b433/*hang.log`. reVC/librw PSP comparison
(2026-11-21): it also drops its baked list for objects that need clipping; its spatial 8x8 triangle cells could
shorten our guard loop (`gloop`+`gsub` ~2 ms) once all crossing objects share one path.

Next candidates: deferred drawing of opaque static lists (keep the renderer hot in the icache instead of
interleaving with game code per atomic), animation (`qmodel`/`aadv` ~9 ms) on the Media Engine.

## b429 GE experiments for PSP A/B

- `p4_clear_stripes=1` uses 32-pixel stripes for RGBA8888 and 64-pixel stripes for 5551 RTT. It covers camera
  and capsule clears, TV stencil resets, and matching untextured solid fullscreen quads. At `0`, camera
  clears use `sceGuClear(bits)` without `GU_FAST_CLEAR_BIT` (one sprite in this SDK), capsule and TV fills use
  one sprite, and fullscreen quads use the original two triangles. The SDK's 64-pixel loop requires the fast bit.
- `p4_chain_strips=1` saves VTYPE, BASE and VADDR when adjacent PRIMs draw non-indexed triangle strips with
  the same format and contiguous vertices in increasing address order. Recorded lists and object lists are
  excluded. Consecutive buffers produced by `p4_flat_all` grow downwards and do not qualify.
- `p4_compact_immediate=1` packs the raw non-indexed through format with float UV/position and 8888 colour
  when all coordinates are integers (24 to 16 bytes per GE vertex). It adds CPU validation, a packed copy and a
  VTYPE restore. The 3D path is Im3D only: scaled S16 positions and U16 UVs when eligible. CPU clipping keeps
  its existing format. Measure CPU and GE separately and check texture edges.
- `p4_state_call=1` writes the block, hashes it and searches up to 64 immutable slots without eviction. On a hit
  it replaces 4–40 inline words with BASE and CALL. This reduces words in the main list and adds CPU work and
  a GE CALL/RET. The recorded draw lists retain inline state.

The final three controls default to `0`. PSP A/B results are pending for all four controls. Check
`GE experiments` for coverage before comparing timings.

b433 distinguishes the pipeline gate's reserved SDK stack level from actual GE object/list recording.
The b432 Margaret runs reported zero strip chains and state CALL hits with double lists enabled, so they
did not measure those draw paths. b433 logs format, recording, adjacency, cache and compact-coordinate
fallbacks alongside coverage. The 2D path still requires integral coordinates and does not round the UI.

## How to work

Set `PSPDEV` to your compiler root, `P4_HOST` to a local PSPLink host folder and `P4_SESSION` to a separate local
session folder. These are machine-specific environment variables; paths below are relative to those folders.

- Edit sources on the share. Build with `python tools/local_build.py` with `PSPDEV` set, and add `--census` after
  any change to the FIXES in `tools/psp_census.py`. Game objects come from `build/obj`, not from `psp_hw_debug.py`.
- Bump `#define BUILD` in `port/main.c` for every build that goes to the device.
- Deploy through the PSPLink queue (`${P4_SESSION}/commands.txt`): copy the PRX and data, copy them back and compare
  sha256, `reset`, then `ldstart` in a separate step. `profmode t` survives `reset`.
- A/B: poke switches at `0x08804000 + ELF offset` (`psp-nm build/psplink_bNNN/p4.elf`), `dcache w`, read the value
  back, then take 3 or more `hwperf` windows per arm (`tools/psp_probe_ab.py`; keep its DEFAULTS in step with the
  switch defaults).
- `.bss` must end below `.p4image` (0x809a00): put new large arrays on the heap.
- Grep the decompilation in the local copy (`ext/Persona4-Decompilation`, also `ext/Izanagi` for the PC port's
  overrides by address); over the share it times out.
