# b494 release validation (2026-11-24)

b494 is b493 plus three fixes: FPU traps off on every game thread (issue #19), deferred freeing of texels the GE may still read, and the VAG encoder moved into `helpers/p4meshopt.dll`. Host checks below do not establish PSP gameplay acceptance: the b494 XMB executable has not been run on a PSP in this release session, and PPSSPP does not emulate FPU traps, so the issue #19 fix needs a PSP to confirm.

## Frozen package checks

All checks used the frozen `Prepare P4.exe` from `P4-PSP-b494-windows-x64.zip` and the original USA ISO (Redump 5576).

- `--cli --check --probe`: all five containers recognised (DATA, BTL, ENV, BGM, ZZZZZ); one ADX and one AHX sample converted; 30 H.264 frames encoded from `P4CT020`. Both audio samples are byte-identical to the b493 utility's output (the encoder moved into the DLL).
- `--cli --build-only` into an empty build cache, with the downloaded toolchain and pinned upstream source: every C input compiled (773 + 97 + 498 + 21 + 27 files, 0 failures); 19 PSP import libraries validated; 0 function pointers without a native target. The utility generated and installed EBOOT, data, strings, skin/static strips, the CLUT pack and the static geometry pack.
- Public documentation check (`test_public_docs.py`) passed on the published snapshot.

## Build artifacts

These hashes are from this host build. They have not been matched against a PSP-tested reference.

| File | SHA256 |
|---|---|
| `EBOOT.PBP` | `244857f1a6ee7cca4d26870c954debf26152e91aebf7b4f9b179b3c5b9fea10b` |
| `p4data.bin` | `967617be480900a80734c83f004defd74a3502bb2e27a98ebeee434c2225527b` |
| `p4str.bin` | `4bc65004bf091811ee679342eed10efe1dea01ef0fd02c0e189da45adf6e7ee4` |
| `p4strips.bin` | `631374991fa230abbb9b914c40a89afb9eca667230f3e1de857067a36cca6075` |
| `p4clut.bin` | `424480fe03a146ee0f44fcb14ddf44287b929931764e536dc07631a608957746` |
| `p4static.bin` | `2efa43dc4485ddbd84baa0d165ba1b65401add81c108fbb0621ff4d916c90ff1` |

## Not covered

- The UNDUB ISO was not re-tested in this session.
- Full audio and movie conversion and a complete playthrough were not repeated.
- These still need a PSP check: issue #19 (Junes food court dialogue), plus everything listed as open for b493.
