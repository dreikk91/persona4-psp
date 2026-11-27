# b524 release validation (2026-11-27)

b524 is b495 plus: the 2D images prescaled offline (`p4sprite.bin`), skin entries cached on the memory stick, the caches moved into an 8 MB arena with more game heap and a 2.5 MB reserve for issue #23, crash-catcher register dumps, a heap guard on free and on TMX copies, and XMB pictures made from the player's ISO (or the player's own pictures) instead of shipped artwork. The utility now checks GitHub for a newer package when it opens (see `prepare_game.md`). Host checks below do not establish PSP gameplay acceptance: the b524 XMB executable has not been run on a PSP in this release session.

## Frozen package checks

All checks used the frozen `Prepare P4.exe` from `P4-PSP-b524-windows-x64.zip` and the original USA ISO (Redump 5576).

- `--cli --check --probe`: all five containers recognised; one ADX and one AHX sample converted; a short H.264 sample encoded from `P4CT020`.
- `--cli --build-only` with the downloaded toolchain and pinned upstream source: every C input compiled (773 + 97 + 498 + 21 + 27 files, 0 failures); the utility generated and installed EBOOT, data, strings, skin/static strips, the CLUT pack, the static geometry pack and the sprite pack (1218 images, 331 s).
- The first b524 package hung in the sprite pack: its pool workers could not load `__main__.job` in the frozen executable (the b389 `psp_strips` cause). `psp_sprites.py` now hands the pool the function by module name; the package was rebuilt and both checks above were repeated on it.
- The EBOOT's image slots: ICON0 20219 bytes, PIC1 168980 bytes (both made from the ISO title screen), ICON1 and PIC0 empty.
- Update check: a b494-labelled run found b495 on GitHub, downloaded it, matched `SHA256SUMS.txt` and unpacked a working `Prepare P4.exe` beside the old folder.
- Public documentation check (`test_public_docs.py`) passed on the published snapshot.

## Build artifacts

These hashes are from this host build. They have not been matched against a PSP-tested reference.

| File | SHA256 |
|---|---|
| `EBOOT.PBP` | `4a891e16944c06ef5c477e247d3226ebd7d8e990603a9a33fab8bed5fa9c5f17` |
| `p4data.bin` | `395854e492c0f17274a7c3ef183eac45b297b734274f6e8a70dfa8332e9a984c` |
| `p4str.bin` | `4bc65004bf091811ee679342eed10efe1dea01ef0fd02c0e189da45adf6e7ee4` |
| `p4strips.bin` | `631374991fa230abbb9b914c40a89afb9eca667230f3e1de857067a36cca6075` |
| `p4clut.bin` | `424480fe03a146ee0f44fcb14ddf44287b929931764e536dc07631a608957746` |
| `p4static.bin` | `2efa43dc4485ddbd84baa0d165ba1b65401add81c108fbb0621ff4d916c90ff1` |
| `p4sprite.bin` | `5cc91805b0571ca34027a23230c210a87756c1a1414038467a6576d7fb2caeb3` |

## Not covered

- The UNDUB ISO was not re-tested in this session.
- Full audio and movie conversion and a complete playthrough were not repeated.
- The update prompt in the desktop window was not clicked through; the download/verify/unpack path it calls was tested from the command line.
- These need a PSP check: the heap changes for issue #23, the skin cache on the memory stick, the sprite pack, and the crash-catcher register dump.
