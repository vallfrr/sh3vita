# Third-party notices — sh3vita

The port uses third-party software. Its authors retain their copyrights and the applicable terms below. These notices do
not claim ownership of Silent Hill 3, its executable, resources or generated translations. Players supply their own
supported installation.

## Graphics and shader services

- **vitaGL**, Rinnegatamante and contributors: [source](https://github.com/Rinnegatamante/vitaGL). Source files identify LGPL version 3 or later.
  See `LICENSES/LGPL-3.0.txt` and `LICENSES/GPL-3.0.txt`. The sh3vita modifications are supplied as a patch, `port/patches/vitagl-sh3.patch`,
  against commit `2bdbe895ddc6a7e410a9ea55a8f53d8173c61ad8`; the exact patch/library combination must be matched to each release.
- **vitaShaRK**, Rinnegatamante and contributors: [source](https://github.com/Rinnegatamante/vitaShaRK), LGPL version 3 or later. Its use is separate
  from the user's proprietary `libshacccg.suprx`, which this project does not supply.
- **SceShaccCgExt**: [source](https://github.com/Rinnegatamante/SceShaccCgExt), GPL version 3. It is linked into the executable, which is therefore
  distributed under GPL-3.0 with the corresponding sources in this repository.

## Bundled and linked support libraries

- **pl_mpeg**, Dominic Szablewski: [source](https://github.com/phoboslab/pl_mpeg), MIT. The bundled copy has port modifications
  (MPEG audio Layer I decoder) and must not be represented as an unchanged upstream version. See `LICENSES/short-licenses.txt`.
- **TLSF**, Copyright (c) 2006–2016 Matthew Conte: [source](https://github.com/mattconte/tlsf), BSD 3-clause; see `LICENSES/short-licenses.txt`.
- **SDL2**, Sam Lantinga and contributors: [source](https://github.com/libsdl-org/SDL), zlib license.
- **math-neon**, Lachlan Tychsen-Smith, Vita port by Rinnegatamante: [source](https://github.com/Rinnegatamante/math-neon), MIT.
- **VitaSDK / vita-headers**, vitasdk contributors: [source](https://github.com/vitasdk/vita-headers), MIT. Compiler runtime, C/C++ runtime, zlib and taiHEN stubs come from the VitaSDK toolchain.
- **FFmpeg**, the FFmpeg developers: [source](https://github.com/FFmpeg/FFmpeg), GPL version 3 builds with libx264 (GPL) and the native AAC encoder.
  The released `sh3prep` binaries embed one static ffmpeg build per platform, taken from
  [eugeneware/ffmpeg-static](https://github.com/eugeneware/ffmpeg-static) release `b6.1.1` and pinned by SHA-256 in `.github/workflows/sh3prep.yml`
  (Windows: gyan.dev "essentials" 6.1.1; Linux: johnvansickle.com static 7.0.2; macOS: the build shipped with that release). It is run as a
  separate program to convert the movies; its source and build information are at the links above and in that release's README/LICENSE assets.
  libx264 is by the x264 developers (GPL-2.0-or-later); the FFmpeg builds as a whole are GPL-3.0.
- **sh3prep** dependencies (`eframe`/`egui`, `rfd`, `sha2`, `flate2`) are listed in `tools/sh3prep/Cargo.lock`; they are dual MIT/Apache-2.0 or MIT licensed.

## Artwork

- Controller screen: Vita outline by Tokyoship (Wikimedia Commons, CC BY 3.0), recolored; button prompts from Kenney Input Prompts (CC0). See `port/assets/controls/`.
- LiveArea images (`port/livearea/`): community artwork from SteamGridDB (icon by FrostWolf, hero by Maxine, logo by kaito), cropped and recolored to the Vita formats. The Silent Hill logo and imagery belong to Konami; see `port/livearea/README.md`. They are not covered by the GPL.

## Ancestry

The Direct3D 8 / DirectX replacement layer under `port/sh4base/` and the recompiler under `recomp/` come from
[sh4vita](https://github.com/vallfrr/sh4vita) by the same author, GPL-3.0.

This notice collection is a work in progress; see `docs/DISTRIBUTION-LICENSE-AUDIT.md`.
