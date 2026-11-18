# Developer workflow

For people who want to rebuild or work on the port. Players only need the README.

## Layout

| Folder | Content |
|---|---|
| `recomp/` | x86 to C recompiler (`x86rec.py`) and its tests |
| `re/ghidra/` | Ghidra script that exports the function list of `sh3.exe` |
| `port/` | The runtime (Win32, Direct3D 8, DirectInput, DirectSound replacements, Vita layer) and the CMake build |
| `port/patches/` | Patch for vitaGL |
| `tools/` | Code generators run by CMake (`*_unit.py`) and the preparer `sh3prep` (Rust) |
| `docs/` | Documentation |

`game/`, `re/out/`, `port/gen/` and `build/` are local and never committed: they hold or derive from the game.

## 1. Generate the C code

```sh
mkdir -p game && cp /path/to/sh3.exe game/sh3.exe
analyzeHeadless <project-dir> sh3 -import game/sh3.exe -scriptPath re/ghidra \
  -postScript ExportFunctions.java re/out/functions.json -deleteProject
python3 recomp/x86rec.py          # Capstone required; writes port/gen/
```

## 2. vitaGL

The port needs vitaGL with depth/stencil preservation between render scenes, plus a small patch.

```sh
git clone https://github.com/Rinnegatamante/vitaGL ~/src/vitaGL
cd ~/src/vitaGL && git checkout 2bdbe895ddc6a7e410a9ea55a8f53d8173c61ad8
patch -p1 < /path/to/sh3vita/port/patches/vitagl-sh3.patch
make -B NO_DEBUG=1 STORE_DEPTH_STENCIL=1 -j"$(nproc)"      # produces libvitaGL.a in the checkout
```

Use `make -B` whenever you change these flags: make does not track them. Other libraries come from the VitaSDK
ecosystem: vitaShaRK, SceShaccCgExt, math-neon, SDL2 for Vita, taiHEN stubs.

## 3. Build the VPK

From Linux or WSL with VitaSDK (`VITASDK=~/vitasdk`), CMake and Ninja:

```sh
SH3_VITAGL_DIR=~/src/vitaGL bash port/build.sh
```

The result is `build/sh3vita.vpk` (title ID `SILHIL003`). The build runs outside the source tree
(`~/build/sh3vita`) because compiling on a Windows drive from WSL is slow.

Optional local bundles derived from the game (never committed) can be packed into the VPK if present:
`build/vpk-bundle/d3dxcache/sh3dxa1.bin` (D3DX shader assembly cache) and
`build/vpk-bundle/shadercache/r1/programs/*.bin` (compiled GPU programs harvested on a console that played the game).

## 4. The preparer

```sh
cd tools/sh3prep
cargo test --release
cargo build --release          # GUI needs libgtk-3-dev libxkbcommon-dev libwayland-dev on Linux
# Optional: embed ffmpeg like the released binaries do. Put a gzip of a static ffmpeg (libx264 + aac) at tools/sh3prep/ffmpeg/ffmpeg.gz
# before building (the release workflow fetches a pinned build). Without it, sh3prep uses the ffmpeg found on PATH or in SH3PREP_FFMPEG.
```

Tests that use real game files run only when you point them at yours:
`SH3PREP_TEST_GAME=<game folder>` and, for the movie decryption, `SH3PREP_TEST_MOVIE=<op.000>` with
`SH3PREP_TEST_MPG=<a known-good decrypted copy>`.

## Developer files on the console

The runtime reads optional marker files in `ux0:data/sh3/diag/` (profiling, capture and experiment switches).
They are inert unless you create them; names and effects are in the sources (`port/diag_runtime.c`).
