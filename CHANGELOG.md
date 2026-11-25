# Changelog

## b495 pre-release (2026-11-25)

Fixes on top of b494. Not yet run on a PSP in this release session.

- When the game archives are missing (an "EBOOT only" preparation), the EBOOT now says which files are missing and how to prepare them, then exits, instead of failing later (issue #20).
- From the persona4-portable fork: the disc archive parts are opened on first read, a truncated or malformed `p4files.idx` is refused, and a frame with nothing drawn no longer shows a stale buffer, and a log line cut at the buffer size still ends with a newline.
- The EBOOT has its own XMB icon and background (ICON0, PIC1).
- Utility: the preparation uses up to 16 workers (the CPU count) instead of 4.

- Crash catcher (issue #21: hard power-offs with nothing in the log). On a CFW with KUBridge (ARK, PRO, ME) the EBOOT writes a small kernel module, `p4crash.prx`, next to itself and loads it; it registers the default exception handler. A crash in the game then writes `p4crash.txt` in the game folder (thread, exception, EPC and `ra` as `p4+offset`, BadVAddr, all registers, the code addresses on the stack, and the last 16 KB of the log including the lines the writer had not saved yet) and returns to the XMB instead of hanging and powering off. The log says `crash catcher: ... handler 00000000 (on)` when it is active. An empty `CRASHTEST` file next to the EBOOT crashes on purpose at boot to check it. PPSSPP: the module loads, the handler is not available there (`(off)`), the game runs as before. Not yet run on a PSP.
- Dialogue portraits no longer hitch when they appear (issue #22). Since b491 every big 8-bit 2D image is resampled once to the PSP's size, and that ran all at once at the first draw: ~0.13-0.3 s of CPU per portrait (PPSSPP clock). It now runs ~2 ms a frame (`p4_swz_step_us`), the original image drawn until the copy is done (~50 frames); one-index windows are copied without filtering (68% of a portrait), and the nearest-colour search looks only at the 4x4 source texels around each output texel instead of all 256 palette entries. PPSSPP, Margaret route: portrait picture unchanged, 126 ms over ~53 frames instead of one stall. Not yet run on a PSP.

## b494 pre-release (2026-11-24)

Fixes on top of b493. Not yet run on a PSP in this release session.

- FPU traps are now off on every game thread, not only the PSP main thread (FCR31 is per thread). With traps on, a NaN compare on a game thread powered the PSP off with no error: the likely cause of issue #19 (hard crash during Chie's dialogue at the Junes food court, PSP-3000). PPSSPP does not emulate FPU traps, so this needs a PSP to confirm.

- Texels and palettes of destroyed rasters, and dropped 2D/CLUT8 copies, are freed only after the GE finishes the list that may still read them. With double-buffered lists, freeing at once let the memory be reused under the GE (possible one-frame texture garbage). Found by the persona4-portable fork's QA (raster/CLUT lifetime). PPSSPP: title, load screen and a field load run clean; battle and PSP not checked yet.
- Utility: the VAG audio encoder now lives in `helpers/p4meshopt.dll`; the separate `audio_vag_encoder.exe` (flagged by some antivirus software) is gone. Its output is byte-identical, so converted audio caches stay valid.

## b493 pre-release (2026-11-23)

Port and utility changes since b450. "PSP" figures come from profiling builds at 333 MHz on the named scene; the b493 XMB executable produced by the utility has not itself been run on a PSP in this release session.

### Changes retained (on by default unless noted)

| Builds | Change | Evidence / result |
|---|---|---|
| b451 | Offline static geometry pack (`p4static.bin`): integer s8/s16 vertices, meshoptimizer simplification and strips, conservative bounds, relocatable GE templates | The utility generates and installs the pack (about 54 MB on the memory card). Runtime consumer **off** (`p4_static_pack=0`) pending PSP validation. Details: [offline static geometry](docs/offline_static_b451.md). |
| b453-b461 | Field passes draw prepared-list atomics from flat arrays instead of walking RenderWare (`p4_world_direct`) | PSP: Heaven 41.0 -> 37.9 ms CPU, Junes West Entrance 52.8 -> 44.4 ms; RW callbacks 210 -> 47 a frame; pictures unchanged, check mode 0 list diffs. |
| b462-b463 | Gated lists in the direct path (`p4_static_gate`, **off**); mid-game route replay for testing | Clouds gain little: their chunks all cross the guard band. |
| b464-b472 | Field collision skips atomics a query cannot reach and limits big geometries to the triangles a world grid allows (`p4_col_cull`) | Heaven bridge run: collision 4.0 -> 0.7 M cycles a frame, CPU 62 -> 53 ms; check mode 0 diffs. |
| b473 | Bone frames written only when read (`p4_frame_lazy`) | Heaven standing: -0.6 ms CPU; dialogue -1.5..1.7 ms; no lost frames. |
| b481 | Direct field entries queued and emitted together in retail order (`p4_world_defer`) | Heaven: instruction misses 38.2k -> 36.6k a frame, about -0.4 ms CPU; picture unchanged. |
| b482-b493 | Title screen and 2D: swizzled copies of indexed 2D textures the game writes itself (`p4_swz_copy`); layers of 256x256 and larger prescaled to the PSP size with Lanczos-2, smaller images kept at level 0 | Title (PRESS ANY BUTTON): 10 FPS -> ~30 FPS (GE wait 55-60 -> ~6 ms, b490 measurement). Battle HP gauges and text kept sharp. |
| b485 | Movie and audio restart run in watched threads; 4 s without progress skips the movie instead of freezing | Mitigates Media Engine RPC hangs ([notes](docs/audio_video_hang.md)); title attract loop of 7 movies without a stall. Not proof that every hang is gone. |

### Fixes

- b486, b489: battle HP gauges read empty: textures the game locks keep their original, and small 2D copies have no mip levels.
- b488: freeze after a battle or a field change (bus error in the world-direct report reading freed geometries).
- b491-b492: blurred menu and save-screen text from prescaling small 8-bit images.

### Experiments left off

- b480 native effect rotation helpers (`p4_eff_rotate`): exact over ~38k calls, no gain.
- b474-b479 profiling found Heaven's orbit particle emitter costs ~0.65 M cycles a frame; not yet optimised.

### Validation and limitations

See [b493 validation](docs/build_validation_b493.md). Title-screen and battle-gauge pictures with the final b493 copies, movie hangs, heap fragmentation after scene changes and slower heavy scenes still need PSP checks. No complete playthrough or stable whole-game 30 FPS is claimed.

## b450 pre-release (2026-11-21)

Source-build utility and port changes since b388. Hardware results below come from the named diagnostic builds at 333 MHz; the b450 XMB executable needs its own gameplay validation. A smaller buffer or faster CPU section does not imply an FPS gain.

### Changes retained

| Builds | Change | Evidence / result |
|---|---|---|
| b389 | Whole-block texture swizzling, one flush per mip chain, allocation rollback | Adapted from MeMeNT0xMoRii/persona4-portable; avoids partial raster allocation on failure. |
| b390-b411 | Weight-group skin batches, flat vertex streams, compact vertex colours; static lists in render targets | Current rendering paths; scene coverage and benefit vary. No release-wide FPS claim. |
| b405 | PSP Go launch/storage handling; Rush/Stop bar placement | Fork adaptations retain attribution in source. |
| b424-b427 | MPEG firmware library allocated below 0x0A000000; source release linker fixed | Address-placement mitigation; movie hangs recurred in b433, so this is not a confirmed general fix. |
| b436-b445 | GE-native static lists: compact s16/s8 positions, strips, bounding boxes and uniform-colour handling | Heaven PSP: list data 2.65 -> 1.62 MB, picture unchanged; CPU unchanged. Enabled. |
| b439 | Static list keys use only the generations of rasters actually bound | Stops unrelated font texture updates from rebuilding room lists; prior dialogue rebuild spikes reached 445 ms. |
| b446 | Atomic rendering hot code grouped together | PSP: atomic instruction misses 17.6k -> 15.6k, about 0.4 ms saved in that section; total frame misses unchanged. |
| b448-b449 | 2D state cache enabled, texture-only path and miss diagnostics | Heaven result within noise; dialogue coverage still needs measurement. |
| b450 | Optional direct sprite/glyph state and GE strip path | Default off (`p4_ui_native=0`); requires PSP image/time A/B. |

### Short PSP FPS results

Recorded hardware comparison from the b390-b400 optimization session (333 MHz, profiler off); this is historical evidence, not a fresh b388-to-b450 A/B.

| Scene | Before | After (b400) |
|---|---:|---:|
| Entrance | ~15 FPS | ~23 FPS |
| Junes West Entrance | ~15 FPS | ~23 FPS |
| Heaven, 10th Paradise | ~15 FPS | Not confirmed |

The earlier Paradise 30 FPS report was withdrawn after the test scene was identified as Junes.

### Experiments that did not justify enabling

- Guard disabled: about 42 versus 51 ms CPU while running in Heaven, but cloud smearing and battle floor holes. Kept enabled.
- Long-edge subdivision: clouds improved in one route, but battle geometry grew from 8.4k to 164k triangles, reached 7.5 FPS and exhausted the list/heap budget. Experimental splitting remains off.
- Static chunks, gate lists and guard snapping: no useful measured gain on the tested route; off.
- Constant-pose animation shortcut: no gain in field 63, slower; removed.
- Offline static strips and CLUT packs: very little coverage in Heaven (22 static hits, one texture), no measured frame gain. Their consumers remain off.

### Preparation utility

- Accept the original USA ELF and the tested USA UNDUB ELF by full SHA-1. Unknown executable modifications remain rejected. The selected disc supplies its own voice/movie/container data; installations from different ISOs must use separate output folders.
- Generate both skin and static mesh strips (`p4strips.bin`) and offline texture palettes/indices (`p4clut.bin`) from that ISO. Include the converter sources, frozen Pillow runtime and license, verify both pack hashes, and install them alongside EBOOT in both build-only and full preparation.
- Compact static GE command lists still compile at runtime. The standalone LOD producer has no runtime consumer in b450 and is not part of automatic preparation.
- Source snapshot and utility omit external checkouts, other-game decompilation sources/research notes, game assets and generated executables. Retain fork credits in README, notices and source.

### Validation and limitations

See [b450 validation](docs/build_validation_b450.md) for the actual frozen-package checks. Selected older hardware measurements are in [the b366 audit](docs/hardware_b366.md). Movie hangs, heap fragmentation after scene changes, fusion glow flicker and slower heavy scenes remain open. No complete playthrough or stable whole-game 30 FPS is claimed.

## Earlier b354-b388 release

Builds b354-b388 (branch `perf/offline-mesh`, merged into `main` on 2026-11-18), published as the b388 pre-release.
Each entry lists what was tried and what it gave. "PSP" means a real PSP at 333 MHz
with the profiler off. "PPSSPP" figures are emulated cycles: they show where the work is,
not how long it takes on a device. The test scene is the Margaret dialogue
(b360-b366 PSP: CPU about 48 ms, about 67 ms a frame, 14.8 FPS) unless another scene is named.

## Kept on

| Build | What was tried | Result |
|---|---|---|
| b365-b366 | GE words (state, texture binds, matrices, light, blend, stencil, fog, draws) written directly instead of through libpspgu (`p4_ge_raw`) | PSP: off costs about 4 ms CPU and 75.7-77.7 ms a frame. **On.** |
| b383 | RenderWare cull mode for lit untextured meshes without prelight (`p4_cull_back 3`) | The limousine's blue floor patch is gone on the PSP. Culling every 3D mesh (1/2) cut holes in curtains and walls. **3 on.** |
| b385 | Skin guard: a bone whose sphere leaves the guard band is tested again with the box around its vertices (`p4_skin_guard_box`) | Velvet Room, Satan after a fusion, PSP: 41.6 ms CPU / 24.0 FPS -> 30.8 ms / 29.7 FPS, 0 fallbacks. **On.** |
| b386 | Back-face culling for every Im3D draw with cull mode 2 (`p4_im3d_cull`) | Fusion cards show their faces on the PSP. **On.** |
| b377, b388 | CLUT8 copies of 32-bit textures (`p4_clut32`) | Cloud plane as CLUT8: GE after the CPU 26.1 -> 21.1 ms, same picture (b371). b388 extends it to every 32-bit texture (min size 0) with a hashed slot table (256 slots, 8 probes). The copy costs about 0.34 s per 256x256 at first bind: to move offline. **On.** |
| b388 | Guard cut skipped for geometry whose triangles are short relative to their depth (`p4_guard_big 1.0`) | PSP b366: 256 uncut, 0 cut, no frame change in the dialogue. Default on since b388; check near the camera in other scenes. |
| b388 | Characters skip the bone-sphere guard test (`p4_skin_guard 0`) | PPSSPP: sguard 241k -> 185k cycles a frame. Default 0 since b388; may leave small holes when the camera is inside a character. |
| b366 audit | Triangle strips for skin batches and strips packed on the PC (`p4_skin_strip`, `p4_strip_pack`) | PSP: strips off 83.4 ms a frame (12.0 FPS), pack off 78.4 ms (12.75 FPS), against about 67.6 ms. **Both stay on.** |
| b366 audit | Model preparation skipped in skipped ticks (`p4_skip_model_lite`), bone skipping in skipped ticks (`p4_frameskip_bones`) | Off costs about 3.7 ms and about 4 ms CPU. All animation shortcuts off together: 61.6 ms CPU / 78.1 ms a frame. **On.** |

## Measured and left off

| Build | What was tried | Result |
|---|---|---|
| b364 | Double-buffered command lists: the GE runs list N whole while the CPU writes N+1 (`p4_list_double`) | PSP: 47.9 ms CPU / 67.3 ms a frame, inside the baseline range. **Off.** |
| b358 | RenderWare frame sync keeps the bone LTMs the pose just wrote (`p4_sync_skip_fresh`) | PPSSPP check: 0 differences, about 430-670 multiplies a frame saved. PSP: -0.6 ms CPU, no frame gain. **Off.** |
| b360-b361 | Bone frames written only when read (`p4_frame_lazy`) | PPSSPP: kmat 217k -> 148k cycles, same picture. PSP: -1.5 to -1.7 ms CPU, no frame gain. **Off.** |
| b359 | MatFX env pass on the GE for GE-skinned characters (`p4_matfx_env_ge`) | Not exercised: the tested scenes have no skinned env-mapped atomics. **Off.** |
| b377-b378 | The port's caches in their own 12 MB arena (`p4_arena_kb`) | PSP, field 20: the arena filled up (largest free block 4 KB, about 7400 refused allocations a window), static lists re-recorded every frame, 106 ms a frame. **Off** until every cache has a budget that fits. |
| b379-b384 | A frame the game does not clear starts from the frame shown before it (`p4_keep_previous`) | Fixed the fusion-glow flicker on the PSP, but the GE cost in Heaven and field 20 (90-109 of 128 frames) was not measured. **Off again** (b384); the flicker is open. |
| b366 audit | Lazy interpolation, far animation, held AddAnimTime, native AddAnimTime alone | About 1 ms CPU or less each, no clear frame gain. Lazy and far stay on as before. |

## Diagnostics added

| Build | Tool | What it showed |
|---|---|---|
| b354-b355 | Timing of static-list recording; draws without a list counted by cause; histogram of guard-cut triangles | The ceiling of an offline (PC-built) static pack, and why lists are refused. |
| b356 | Profiler sections on the system clock without the thread profiler (`p4_prof_clock`) | Where the work is in PPSSPP. |
| b358 | Bone frame census (`p4_frame_census`) | About 640 bone frames written a tick, about 35 read. |
| b362 | Probe bits 128/256: hide only characters or only scenery | PSP GE alone about 26 ms: 8 characters 10.6 ms, 2D 8.4 ms, translucent cloud and floor planes and 84 flowers make most of the rest. |
| b363 | Per-frame CPU/GE timeline (`p4_timeline`) | CPU 47.8 ms and GE alone 26 ms, yet 67 ms a frame: they do not overlap. See [docs/ge_frame.md](docs/ge_frame.md). |
| b377 | Heap map (`p4_heapmap`), 2D texture census | The bus error after battles: a 2 MB request with 18 MB free in pieces, largest 1.4 MB. |
| b387 | Guard cut census per geometry (`p4_guard_census`), camera near/far in the log | Battle 512 cuts 1400-7300 triangles a window from two geometries (long edges about 6700 and 1100); Junes 0. Input for offline tessellation. |
| b388 | `p4_guard_tiny`: skip the cut for tiny geometry at any depth | Off; pick the length from the b387 histogram. |

## Preparation utility

- Build-cache paths stay under 250 characters: the job folder is named by 16 hex characters instead of 64,
  and the build stops early with a clear message when the longest workspace path plus 24 characters for
  compiler include joins would pass 250 (a deep custom cache folder). The freeze step reports its longest
  temporary path through the same check.
- The utility builds the character strip pack (`p4strips.bin`) from the player's ISO and installs it next to the
  EBOOT. Earlier packages never made it, so their EBOOT built strips on the PSP: 78.4 ms a frame against 67.6 ms
  with the pack (b366 audit). The frozen executable's process pool hung until `freeze_support` was called and the
  task function was passed by module name.
- `pack-pbp` takes ICON0/PIC1 from a local, git-ignored `build/icons/` when present. No artwork is
  shipped; packaged builds without icons stay byte-identical to the reference.

## Documentation

- [docs/hardware_b366.md](docs/hardware_b366.md): the b366 PSP audit (34 arms, 13,056 frames).
- [docs/cpu_live_b366.md](docs/cpu_live_b366.md): live CPU capture.
- [docs/animation_psp.md](docs/animation_psp.md): P4 animation costs and pose-path measurements.
- [docs/runtime_controls_b366.md](docs/runtime_controls_b366.md): all 230 runtime switches (translated to English).
- [docs/ge_frame.md](docs/ge_frame.md), [docs/visual_issues.md](docs/visual_issues.md): GE frame and fixed visual issues.
