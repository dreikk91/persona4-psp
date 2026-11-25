# b495 release validation (2026-11-25)

b495 is b494 plus: the crash catcher (`p4crash.prx`, `p4crash.txt`; issue #21), dialogue portraits prescaled over frames instead of at once (issue #22), a clear message when the game archives are missing (issue #20), three fixes from the persona4-portable fork (lazy archive opening with `p4files.idx` validation, no stale frame when nothing was drawn, log line endings), the XMB icon and background, and up to 16 preparation workers. Host checks below do not establish PSP gameplay acceptance: the b495 XMB executable has not been run on a PSP in this release session. PPSSPP cannot exercise the crash catcher (the module loads there, the exception handler call is not available), so it needs a PSP check with the `CRASHTEST` switch file.

## Frozen package checks

All checks used the frozen `Prepare P4.exe` from `P4-PSP-b495-windows-x64.zip` and the original USA ISO (Redump 5576).

- `--cli --check --probe`: all five containers recognised (DATA, BTL, ENV, BGM, ZZZZZ); one ADX and one AHX sample converted; 30 H.264 frames encoded from `P4CT020`. Both audio samples are byte-identical to the b494 utility's output.
- `--cli --build-only` into an empty build cache, with the downloaded toolchain and pinned upstream source: every C input compiled (773 + 97 + 498 + 21 + 27 files, 0 failures); 20 PSP import libraries validated (KUBridge is new); 0 function pointers without a native target. The utility generated and installed EBOOT, data, strings, skin/static strips, the CLUT pack and the static geometry pack.
- The EBOOT's image slots: ICON0 31994 bytes, PIC1 243523 bytes, ICON1 and PIC0 empty. A first b495 package put PIC1 into the ICON1 slot; it was rebuilt with the fix before these checks.
- PPSSPP (development build of the same sources): boot to Heaven on the Margaret route with the crash catcher loaded (`handler 8002013a (off)`); the 4/14 day request ran through the Junes TV scene into the TV world without a crash (issue #21's spot; PPSSPP does not show that crash).
- Public documentation check (`test_public_docs.py`) passed on the published snapshot.

## Build artifacts

These hashes are from this host build. They have not been matched against a PSP-tested reference.

| File | SHA256 |
|---|---|
| `EBOOT.PBP` | `fa6ef9f7690673aa4b7e7b1cf97e4fa66badc9b398cc3da67eaec7997c7ae73e` |
| `p4data.bin` | `b2d12b1599b7a78c0496a8ba777eb09678ee2e4cbc1916c222bcfc42cbc8eb43` |
| `p4str.bin` | `4bc65004bf091811ee679342eed10efe1dea01ef0fd02c0e189da45adf6e7ee4` |
| `p4strips.bin` | `631374991fa230abbb9b914c40a89afb9eca667230f3e1de857067a36cca6075` |
| `p4clut.bin` | `424480fe03a146ee0f44fcb14ddf44287b929931764e536dc07631a608957746` |
| `p4static.bin` | `2efa43dc4485ddbd84baa0d165ba1b65401add81c108fbb0621ff4d916c90ff1` |

## Not covered

- The UNDUB ISO was not re-tested in this session.
- Full audio and movie conversion and a complete playthrough were not repeated.
- These need a PSP check: the crash catcher (`crash catcher: ... (on)` in the log, then `CRASHTEST` returns to the XMB with a `p4crash.txt`), the portrait hitch (issue #22), the Junes transition on 4/14 (issue #21), plus everything still open for b494.
