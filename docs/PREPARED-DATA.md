# Prepared data

## What the player does

Players prepare their own installation once with `sh3prep` and copy the resulting `sh3/` folder to `ux0:data/sh3`.

- **Input:** the folder that contains `sh3.exe` (3,338,240 bytes, SHA-256 `a51f956bd5be21fd704c4d19cf0674a175d002081da43b1d377be83226c36e5e`) and `data/`, including `data/movie/*.000`. Anything else is refused before any file is written.
- **Output** (`sh3/`):

```text
sh3.exe             copy of the player's executable (the VPK's recompiled code depends on it byte for byte)
data/               copy of the player's data folder
movie/<name>.mpg    the seven movies, decrypted (software decoder)
movie/<name>.mp4    the seven movies, converted for the Vita's hardware decoder (ffmpeg is built into sh3prep)
sh3vita.json        manifest: executable hash, file counts, tool version
```

Preparation is repeatable: files already present at the right size are skipped (the executable is re-checked by hash),
saves and settings in `savedata/` are never touched. `savedata/`, `cache/` and `diag/` are created by the game on the Vita.

## Movies

The seven PC movies (`op`, `ed_b`, `ed_u`, `ed_n`, `henshin`, `huxx`, `muxx`) are MPEG program streams obfuscated with a
32-bit LCG keystream (the scheme is described in `tools/sh3prep/src/movie.rs`). `sh3prep` removes the obfuscation and writes the plain stream
to `movie/<name>.mpg`. The decrypted output was compared byte for byte with an independent reference decoder.

With ffmpeg (embedded in the released sh3prep binaries, otherwise the one installed on the computer), each movie is also converted to H.264 (baseline, level 3.1, CRF 18) and AAC-LC 48 kHz stereo,
which the Vita's AvPlayer decodes in hardware at 30 fps. If no ffmpeg can be run the `.mp4` files are skipped and the game falls
back to the built-in software decoder (about 20 fps). The converted files are derived from game data: never share them.

## What the VPK contains

- the port executable (recompiled game code plus the runtime),
- optionally, a versioned cache of compiled GPU programs and of D3DX shader assemblies (`shadercache/r1/`, `d3dxcache/`), harvested on
  a console and read on demand; missing entries are compiled at run time and cached in `ux0:data/sh3/cache/`,
- LiveArea artwork (community images, see `port/livearea/README.md`), a launch screen and the controller screen.

It does not contain the original executable, textures, movies, sounds or saves.
