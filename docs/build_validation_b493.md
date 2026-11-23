# b493 release validation (2026-11-23)

The pre-release contains source and a Windows source-build utility. Host checks and the frozen ISO-to-EBOOT build below do not establish PSP gameplay acceptance: the b493 XMB executable has not been run on a PSP in this release session. PSP figures in the changelog come from the named profiling builds.

## Frozen package checks

All checks used the frozen `Prepare P4.exe` from `P4-PSP-b493-windows-x64.zip` and the original USA ISO (Redump 5576).

- `--cli --check --probe`: all five containers recognised (DATA, BTL, ENV, BGM, ZZZZZ); one ADX and one AHX sample converted; 30 H.264 frames encoded from `P4CT020`.
- `--cli --build-only` into an empty build cache, with the downloaded toolchain and pinned upstream source: every C input compiled (773 + 97 + 498 + 21 + 27 files, 0 failures); 19 PSP import libraries validated; 0 function pointers without a native target. The utility generated and installed EBOOT, data, strings, skin/static strips, the CLUT pack and the static geometry pack. As expected for build-only mode, no game resources were converted.
- The package carries 85 source payload files with SHA256 hashes in `prepare.json`.
- Public documentation check (`test_public_docs.py`) passed on the published snapshot.

The static geometry pack (`p4static.bin`, 18,426 entries, about 54 MB) is installed, but its runtime consumer stays off (`p4_static_pack=0`). It takes memory-card space only.

## Build artifacts

These hashes are from this host build. They have not been matched against a PSP-tested reference.

| File | SHA256 |
|---|---|
| `EBOOT.PBP` | `268f1e6d98074bc89d57e71aadcc06cfe99d05dbea6c36827be4a6775228a429` |
| `p4data.bin` | `ca7e3abfdcaad705a9c5d76cfb8e0797a8e2f233a9390725995fb8366a08235e` |
| `p4str.bin` | `4bc65004bf091811ee679342eed10efe1dea01ef0fd02c0e189da45adf6e7ee4` |
| `p4strips.bin` | `631374991fa230abbb9b914c40a89afb9eca667230f3e1de857067a36cca6075` |
| `p4clut.bin` | `424480fe03a146ee0f44fcb14ddf44287b929931764e536dc07631a608957746` |
| `p4static.bin` | `2efa43dc4485ddbd84baa0d165ba1b65401add81c108fbb0621ff4d916c90ff1` |

## Not covered

- The UNDUB ISO was not re-tested in this session; its support is unchanged since b450.
- Full audio and movie conversion and a complete playthrough were not repeated.
- These still need a PSP check: the final b493 title-screen picture and FPS, the battle HP gauges, movie hangs, and heap fragmentation after scene changes.
