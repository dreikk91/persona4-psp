# License scope and third-party notices

## Scope of the project license

The root MIT license applies only to original source and documentation contributions for which this project's contributors hold the necessary rights. Existing third-party notices take precedence for the material they cover. No ownership of third-party material is claimed by adding the root license.

Excluded from that grant are the original game, its code and assets, decompiled or translated retail routines, externally fetched source, proprietary SDK and middleware material, and third-party binaries. Mixed files may contain original porting work alongside reconstructed interfaces or routines; MIT covers only the contributors' own contributions. This file is not a completed provenance audit of every routine.

## Persona 4 decompilation

- Project: [Raikaru/Persona4-Decompilation](https://github.com/Raikaru/Persona4-Decompilation).
- Reference commit: `363eab8a966d60a9773b45f330f2344329ec4f0a`.
- Local location: `ext/Persona4-Decompilation`, excluded from Git.
- Use: recovered C, headers, symbol/provenance maps and RenderWare naming metadata used by the PSP build scripts.
- The inspected checkout has no top-level repository-wide license. Do not describe it as MIT-licensed or treat a public GitHub checkout as a redistribution grant. Its contents are not relicensed by this project.

Retain upstream authorship and any file-level notices. Obtain applicable permission before redistributing upstream material or derivatives where required. Persona 4 game code and assets, Sony code and third-party middleware retain their respective rights.

## Adapted fork contributions

[MeMeNT0xMoRii/persona4-portable](https://github.com/MeMeNT0xMoRii/persona4-portable) is an MIT-licensed fork of this port. Adaptations include launch-folder and PSP Go storage paths, save/data validation, HOME handling, UI/fog/TMX and particle fixes, fusion/Social Stats fixes, and texture swizzling/allocation fixes. Source comments and the development history identify the adaptations. Preserve the fork's MIT notice:

> Copyright (c) 2026 Persona 4 for PSP contributors

See the fork's [LICENSE](https://github.com/MeMeNT0xMoRii/persona4-portable/blob/main/LICENSE) and this repository's MIT license text. This credit covers adapted port contributions, not the underlying game or upstream decompilation.

## Other tools and references

| Component | Use | Upstream |
| --- | --- | --- |
| PSPDEV / PSPSDK | PSP compiler, libraries and packaging tools | [PSPDEV](https://github.com/pspdev), [PSPSDK](https://github.com/pspdev/pspsdk) |
| FFmpeg | Offline audio/video conversion | [FFmpeg](https://ffmpeg.org/) |
| Pillow | Offline texture palette generation | [Pillow](https://github.com/python-pillow/Pillow) |
| PyAV | Video metadata and AVC packaging | [PyAV](https://github.com/PyAV-Org/PyAV) |
| imageio-ffmpeg | Optional FFmpeg executable provider | [imageio-ffmpeg](https://github.com/imageio/imageio-ffmpeg) |
| vgmstream | Original audio decoding, including AHX | [vgmstream](https://github.com/vgmstream/vgmstream), tested release [r2168](https://github.com/vgmstream/vgmstream/releases/tag/r2168) |
| Python, Tcl/Tk, PyInstaller | Optional frozen Windows preparation utility | [Python](https://www.python.org/), [Tcl/Tk](https://www.tcl.tk/), [PyInstaller](https://pyinstaller.org/) |
| PCSX2 widescreen patch, SLUS-21782 | Reference for widescreen changes in `tools/psp_census.py` | pavachan / nemesis2000, as credited beside the adaptations |

These projects are separate dependencies; the root MIT license does not replace their terms. Inspect the exact versions and build configurations you distribute. In particular, FFmpeg licensing and source obligations depend on the features and libraries enabled in its binary.

## Binary packages

The legacy b164 preparation utility bundles third-party programs and an EBOOT. New source-build packages compile the EBOOT locally instead of including it; the upstream checkout is fetched separately. Generated `licenses/` and `THIRD_PARTY.md` files remain an inventory starting point, not proof of redistribution compliance. Preserve applicable license texts, copyright notices, build information and required corresponding source for the exact binaries, including bundled libraries and DLLs and any optionally bundled PSPDEV toolchain. Also include this repository's LICENSE and these scope notices for its own code.

The EBOOT includes game-derived routines. A numerical data recipe or an asset-free package does not settle the rights to distribute that executable. Keep it local until those rights are established. Generated game archives, rebased data, audio and movies are outside the project license and must not be included in a source release.

## meshoptimizer

The offline static geometry compiler uses [zeux/meshoptimizer](https://github.com/zeux/meshoptimizer), v0.25, commit `6daea4695c48338363b08022d2fb15deaef6ac09`, by Arseny Kapoulkine and contributors. The selected source files and original MIT license are in `third_party/meshoptimizer/`. The Windows preparation package includes the host helper DLL and its license; the PSP executable does not link this library.

The host helper is built with GNU g++ and statically links GCC runtime libraries under the GCC Runtime Library Exception. Their GPLv3 and exception texts are preserved in `third_party/gcc-runtime/` and copied into the utility notices.
