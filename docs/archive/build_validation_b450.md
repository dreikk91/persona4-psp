# b450 release validation (2026-11-21)

The pre-release contains source and a Windows source-build utility. The selected USA UNDUB disc has executable SHA-1 `e4b3fac598b1d85906d53389dd38baf9b7375ea1`, 8,621,084 bytes. The original USA ELF remains supported by its full hash. UNDUB changes 11,238 ELF bytes between file offsets `0x46d450` and `0x4d0120`; support does not rely on its filename or accepting arbitrary modified ELFs.

Host checks and frozen ISO-to-EBOOT checks do not establish PSP gameplay acceptance. The final b450 XMB executable has not been tested on hardware in this release session. Historical PSP timings in the changelog identify the diagnostic builds that produced them.

## Host and package checks

- 35 targeted tests passed: preparation (6), source-build/cache/install (15), GE preparation (1), UI drawing/font/sprites (3), audio (3), video (5), memory cards (1), public documentation (1). Font/sprite tests used the external pinned P4 sources and a configured PSPDEV; video tests used the preparer's environment.
- UI checks covered 16,384 native/compatibility draw configurations, 128 glyph cases plus 80 text ticks, and 12,288 sprite cases. These are host equivalence checks, not PSP screenshots.
- Wider checks are not all green: `test_static_meshes.py` cannot find an old source delimiter; `test_static_state.py` extracts code without current list globals; one `test_static_uv.py` fixture lacks current list-key/hot-section definitions; one `test_vu0.py` fixture does not define the profiling symbols `p4_vu0_site` / `p4_vu0_profile`. No whole-suite pass is claimed.
- Frozen `Prepare P4.exe --check --probe` passed on the supplied UNDUB: all five containers recognized, ADX and AHX samples converted, and 30 H.264 frames encoded from `P4CT020`.
- UNDUB catalogue: 17,375 audio streams, 23 movies, zero unsupported audio entries. Full audio/movie conversion was not repeated.
- Public snapshot: 208 source/documentation files reviewed; all 77 utility source payload hashes match the snapshot. No game binaries, external checkouts, saves or dedicated other-game decompilation/research files are included. Public Markdown has no absolute host paths and no broken repository-relative links.

## Frozen build

Clean source compilation through the packaged executable's build workers passed: all 1416 C inputs compiled, 19 PSP import libraries validated, zero function pointers without native targets. The source archive and toolchain fingerprints were verified by the normal build runner. Because the separate CLI was still hashing the complete disc, a host orchestration harness started the same packaged `--build-worker` stages; its only override selected the actual frozen executable for each stage. The overlapping CLI correctly refused the busy build-cache lock. A subsequent independent frozen CLI run checks the completed cache and launch-file installation.

Offline generation scanned 9,192 files / 28,033 geometry occurrences (12,124 unique geometries). The final strip pack has 70,002 entries, 13,721,520 bytes, zero omitted key collisions. CLUT output has 34 textures, 624,448 bytes. The runtime still prepares compact static command lists; this is not a full offline scene compiler.

Historical FPS figures in the changelog were recovered from the saved 2026-11-18 test-session reports, including the subsequent scene-identification correction. No fresh b388-to-b449/b450 three-scene benchmark was found or run.

Verified build artifact SHA256:

| File | SHA256 |
|---|---|
| `EBOOT.PBP` | `d6ddce8f26d8bf4a9143e18e3921d26686cf62a4088bfcc2941e6e924de9e40b` |
| `p4data.bin` | `b9f9ceeca55b93292fdaee708bf41d17239d1364bbe4b970e35a925b15034127` |
| `p4str.bin` | `4bc65004bf091811ee679342eed10efe1dea01ef0fd02c0e189da45adf6e7ee4` |
| `p4strips.bin` | `631374991fa230abbb9b914c40a89afb9eca667230f3e1de857067a36cca6075` |
| `p4clut.bin` | `424480fe03a146ee0f44fcb14ddf44287b929931764e536dc07631a608957746` |

Independent frozen CLI `--cli --build-only` completed successfully on the same UNDUB ISO after the workers released the cache lock. It recomputed the full-disc identity, verified/reused the built artifacts, and installed EBOOT, data, strings, skin/static strips and CLUT. Every installed file matches its manifest SHA256. As expected for build-only mode, no complete-installation marker was written. This confirms launch-file preparation and cache reuse; it does not test the installed EBOOT on PSP.
