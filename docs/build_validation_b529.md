# b529 release validation (2026-11-27)

b529 is b524 plus: the issue #24 fix (a destroyed bone hierarchy is dropped from the lazy-frame table; a new model at a freed address read the dead entry), an audit of the other port tables that keep game pointers (anim states, interpolators, world-direct entries), and the b525-b528 removal of unused runtime switches. See the [changelog](../CHANGELOG.md). Host checks below do not establish PSP gameplay acceptance: the b529 XMB executable has not been run on a PSP in this release session.

## PPSSPP checks

Profiling builds of b529, not the release EBOOT. PPSSPP timings are not performance results.

- `tools/routes/battle_b177.txt`: reached the battle, lazy frames `lost 0`.
- `tools/psp_stress.py` (freed heap blocks poisoned, field warps 63/1, 9/4, 28/2): 9 of 9 warps arrived, no crash, no heap guard hit, world-direct `stale 0`. The same run with the #24 fix taken out faulted in `p4_hanim_update` at the first warp.

## Frozen package checks

All checks used the frozen `Prepare P4.exe` from `P4-PSP-b529-windows-x64.zip` and the original USA ISO (Redump 5576).

- `--cli --check --probe`: all five containers recognised; one ADX and one AHX sample converted; a short H.264 sample encoded from `P4CT020`.
- `--cli --build-only` with the downloaded toolchain and pinned upstream source: every C input compiled (773 + 97 + 498 + 21 + 27 files, 0 failures); EBOOT, data, strings, skin/static strips, the CLUT pack, the static geometry pack and the sprite pack (1218 images, 325 s) generated and installed. The linked release image contains the RpHAnimHierarchyDestroy hook and no profiling-only poisoning.
- The build-only run used the first b529 package; the published one differs only in its README (the release link): `prepare.json` source and helper hashes are identical, and `--check --probe` was repeated on it.
- The EBOOT's image slots: ICON0 20219 bytes, PIC1 168980 bytes (both made from the ISO title screen), ICON1 and PIC0 empty.
- Public documentation check (`test_public_docs.py`) passed on the published snapshot.

## Build artifacts

These hashes are from this host build. They have not been matched against a PSP-tested reference.

| File | SHA256 |
|---|---|
| `EBOOT.PBP` | `6adee074ff117034fbeb80342219b372b6d0bb52234195a8470ec2479c5369e6` |
| `p4data.bin` | `efbf6b3b1638c6efd724f53b1806e11965e97f73a668adb6f58fc9a4ecf58a87` |
| `p4str.bin` | `4bc65004bf091811ee679342eed10efe1dea01ef0fd02c0e189da45adf6e7ee4` |
| `p4strips.bin` | `631374991fa230abbb9b914c40a89afb9eca667230f3e1de857067a36cca6075` |
| `p4clut.bin` | `424480fe03a146ee0f44fcb14ddf44287b929931764e536dc07631a608957746` |
| `p4static.bin` | `2efa43dc4485ddbd84baa0d165ba1b65401add81c108fbb0621ff4d916c90ff1` |
| `p4sprite.bin` | `5cc91805b0571ca34027a23230c210a87756c1a1414038467a6576d7fb2caeb3` |

## Not covered

- The UNDUB ISO was not re-tested in this session.
- Full audio and movie conversion and a complete playthrough were not repeated.
- The update from b524 to b529 through the utility's own check was not clicked through in the desktop window.
- These need a PSP check: the issue #24 fight in Yukiko's castle (f240_001), and everything b524 listed (heap changes for issue #23, skin cache on the memory stick, sprite pack, crash-catcher register dump).
