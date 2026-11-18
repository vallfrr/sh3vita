<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/logo.png">
    <img src="docs/logo-dark.png" alt="Silent Hill 3" width="100%">
  </picture>
</p>

# Silent Hill 3 for PS Vita

Port of the PC version of Silent Hill 3 (2003) to the PS Vita.
You may use your own copy of the game.

**[Download the latest release](../../releases/latest)**: the VPK for the PS Vita and the data preparer for Windows, macOS and Linux.

## What you need

- A hacked PS Vita (HENkaku/Enso), at least 5 GB free on `ux0:`
- Silent Hill 3 for PC can be found on myabandonware, US version needed with the five zip files), with this exact executable: `sh3.exe`, 3,338,240 bytes, SHA-256 `a51f956bd5be21fd704c4d19cf0674a175d002081da43b1d377be83226c36e5e`. No other version works (no PS2 files either).
- `ur0:data/libshacccg.suprx` from your own Vita (ShaRKF00D).
- `sh3vita.vpk` and the `sh3prep` download for your computer from the [Releases](../../releases) page

## Install

Download the VPK and the preparer for your computer:

| Download | Platform |
|---|---|
| `sh3vita.vpk` | PS Vita |
| `sh3prep-windows.exe` | Windows |
| `sh3prep-macos-app.zip` | macOS, Apple Silicon (double-click app) |
| `sh3prep-macos` | macOS, Apple Silicon (terminal) |
| `sh3prep-linux-x86_64.AppImage` | Linux x86-64 (double-click) |
| `sh3prep-linux` | Linux x86-64 (terminal) |

### Windows

1. Install Silent Hill 3 on your PC, or copy an installed game folder.
2. Run `sh3prep-windows.exe`. Select the game's **installation folder**: the folder containing `sh3.exe` alongside `data/`.
3. Choose a separate output folder and your language, then press **Prepare the files**. The language you choose is also the game's starting language (you can change it later in Options).
4. When preparation finishes, continue with [Copy to the Vita](#copy-to-the-vita) below.

The preparer checks the executable, copies your files, decrypts the seven movies and converts them to MP4 for the Vita's video decoder. ffmpeg is built into `sh3prep`: there is nothing else to install.

### Linux and macOS

Copy the installed game folder (the one containing `sh3.exe` and `data/`) from a Windows installation to your computer, then:

1. Launch the preparer for your system. It is not signed by Apple or by a Linux distribution, so the first launch needs one extra action.

   **macOS (Apple Silicon), double-click:** download `sh3prep-macos-app.zip`, open it, then double-click `sh3prep`. macOS will say it cannot verify the developer: right-click (or Control-click) the app, choose **Open**, then confirm. If macOS still refuses, open **System Settings > Privacy & Security** and click **Open Anyway**.

   **macOS, terminal:**

   ```sh
   chmod +x sh3prep-macos
   xattr -d com.apple.quarantine sh3prep-macos
   ./sh3prep-macos
   ```

   **Linux, double-click:** download `sh3prep-linux-x86_64.AppImage`, allow it to run (right-click > Properties > Permissions > **Allow executing as program**, or `chmod +x sh3prep-linux-x86_64.AppImage`), then double-click it.

   **Linux, terminal:**

   ```sh
   chmod +x sh3prep-linux
   ./sh3prep-linux
   ```

   If the window does not open on Linux, install the display libraries (on Ubuntu/Debian: `sudo apt install libxkbcommon0 libwayland-client0`).

2. In the preparer, select the folder containing `sh3.exe` alongside `data/`.
3. Choose a separate output folder and your language, then press **Prepare the files**. Continue with [Copy to the Vita](#copy-to-the-vita).

From a terminal, without the window: `sh3prep <game folder> <output folder> [--language English|French|German|Italian|Spanish]`.

The preparer rejects unsupported game executables (wrong versions or PS2 files).

### Copy to the Vita

1. Install `sh3vita.vpk` with VitaShell.
2. Copy the generated **`sh3` folder** from your chosen output folder to **`ux0:data/`**. USB is much faster than FTP. The result must look like this:

   ```text
   ux0:data/sh3/
     sh3.exe
     data/
     movie/
     sh3vita.json
   ```

3. Start Silent Hill 3 from the LiveArea.

The first launch compiles some shaders on the console, so the first minutes can stutter more than later runs.

Saves and settings go in `ux0:data/sh3/savedata/`. The current log is `ux0:data/sh3/diag/diag.log`; the 10 previous launches are kept as `diag-1.log` (latest) to `diag-10.log`.

### Updating

Close the game and install the new VPK over the existing application with VitaShell. Keep `ux0:data/sh3/`, including your saves and settings. You do not need to prepare the game files again unless a release explicitly says so. If it does, back up your `ux0:data/sh3/savedata/` folder first.

The application ID is now `SILHIL003`. If you installed an earlier test version (`SILH00003`), the new VPK installs next to it: delete the old Silent Hill 3 bubble afterwards. Your saves and settings in `ux0:data/sh3/` are not affected.

## Controls

The button layout is fixed (no remapping). You can review it at any time in-game with **Options → View controls**. Movement uses Silent Hill 3's **2D** control type: Heather walks in the direction you push the stick.

<table>
<tr>
<td valign="top" width="50%">

### Move & camera

| Input | Action |
|:--|:--|
| **Left stick** | Move |
| **Right stick** | Look around |
| **D-pad** | Same as the left stick (move, menus, puzzle pointer) |
| **L** | Look around (search view) |
| **R** | Aim |

</td>
<td valign="top" width="50%">

### Actions

| Input | Action |
|:--|:--|
| **Cross** | Action / examine |
| **Circle** | Flashlight (in game) / back (in menus) |
| **Square** | Run / guard |
| **Triangle** | Inventory |
| **Select** | Map |
| **Start** | Pause |

</td>
</tr>
</table>

In the Options screen, **L** and **R** switch between the pages.

## Status

**Version 1.0.2** The game is playable from start to finish on a real PS Vita, all major and minor bugs encountered by me or the testers are now fixed.

| Area | State |
|:--|:--|
| Gameplay | Menus, intro, new game, loading saves, fights, save points, map, puzzles with the pointer |
| Cutscenes and movies | Sound, music and voices work. The seven movies run at about 30fps |
| Options | 5 languages, brightness, frame rate (60, 30 or unlimited), saved between launches |
| Controls | Fixed layout, see [Controls](#controls) |
| Performance | Most rooms run at 54–60fps. The heaviest hospital rooms drop to 15–30fps |

Measured rooms and test limits: [docs/KNOWN-ISSUES.md](docs/KNOWN-ISSUES.md).

### Known limitations

- **Heavy rooms** (parts of the hospital basement) can drop below 30 fps.
- **Memory is tight.** In very long sessions (3 hours or more) with many area changes, the game can run out of memory. Saving regularly is recommended.
- **"Unlimited" frame rate** makes Heather's movement uneven. Use the default **60** (or 30).
- The **flashlight glow** and **lens flares** are not rendered. The scene stays readable and playable.

## Problems

| Message or symptom | Fix |
|:--|:--|
| "GAME FILES NOT FOUND" | The exe and folders must be directly in `ux0:data/sh3/`. |
| "MOVIES NOT PREPARED" | `ux0:data/sh3/movie/` is missing or incomplete. Run `sh3prep` again and copy the whole `sh3` folder. |
| "SHADER COMPILER MISSING" | Put `libshacccg.suprx` in `ur0:data/` (install it with [ShaRKF00D](https://github.com/Rinnegatamante/ShaRKF00D)). |
| `sh3prep` rejects the exe | Wrong game version. You need the `sh3.exe` with the SHA-256 above. |
| Movies are choppy (about 20 fps) | They were not converted to MP4. Prepare the files again with `sh3prep` (ffmpeg is built in). |
| Crash or the game closes | Open an issue and attach the log of the affected session. After a relaunch it is normally `ux0:data/sh3/diag/diag-1.log`. |
| Missing or corrupted textures | Usually a memory shortage after a long session. Restart the game from your last save. If it happens early, open an issue with the log and the room name. |

Logs are in `ux0:data/sh3/diag/`. The current one is `diag.log`, and the 10 previous launches are kept as `diag-1.log` to `diag-10.log`, newest first.

## Build from source

You do not need any of this to play: the [Releases](../../releases) page has the VPK and the preparers. This section is for people who want to rebuild the port or work on it.

No game file is ever in this repository. You need your own copy of `sh3.exe` (SHA-256 `a51f956bd5be21fd704c4d19cf0674a175d002081da43b1d377be83226c36e5e`). The C code generated from it (`port/gen/`, tens of megabytes) and the analysis output (`re/out/`) are produced on your machine and are never committed.

### How it works

1. **Ghidra** exports the list of functions of the executable (`re/ghidra/ExportFunctions.java`).
2. **`recomp/x86rec.py`** translates every x86 function into C, using that export.
3. **`tools/*_unit.py`** adapt a few generated functions for the console (controls, options, pause menu, effects), each one checked against the SHA-256 of the function it replaces.
4. **`port/`** compiles the generated C together with a runtime that replaces Win32, Direct3D 8, DirectInput and DirectSound (D3D8 runs on vitaGL), and links it into an `eboot.bin` for the Vita, packaged as a VPK. The runtime is shared with [sh4vita](https://github.com/vallfrr/sh4vita).
5. **`tools/sh3prep/`** (Rust) is the separate desktop tool that prepares the player's game files.

### Requirements

| Step | Tools |
|:--|:--|
| Recompiler | Python 3 with [Capstone](https://www.capstone-engine.org/) |
| Function export | [Ghidra](https://ghidra-sre.org/) 12 and JDK 21 |
| Vita build | Linux or WSL (Ubuntu), [VitaSDK](https://vitasdk.org) (`VITASDK=~/vitasdk`), CMake, Ninja |
| Graphics layer | [vitaGL](https://github.com/Rinnegatamante/vitaGL), built from source with the patch in `port/patches/` (see below) |
| Libraries | vitaShaRK, SceShaccCgExt, math-neon, SDL2 for Vita, taiHEN stubs (all from the VitaSDK ecosystem) |
| Preparer | Rust (stable) |

### 1. Put the game exe in place

```sh
mkdir -p game
cp /path/to/sh3.exe game/sh3.exe
```

### 2. Export the functions with Ghidra

```sh
analyzeHeadless <project-dir> sh3 -import game/sh3.exe \
  -scriptPath re/ghidra -postScript ExportFunctions.java re/out/functions.json -deleteProject
```

### 3. Generate the C code

```sh
python recomp/x86rec.py
```

This takes a few minutes and writes `port/gen/`.

### 4. Build vitaGL

The port needs vitaGL with depth/stencil preservation between scenes and the SH3 patch. The build script refuses to link against a vitaGL that lacks it.

```sh
git clone https://github.com/Rinnegatamante/vitaGL ~/src/vitaGL
cd ~/src/vitaGL
git checkout 2bdbe895ddc6a7e410a9ea55a8f53d8173c61ad8
git apply /path/to/sh3vita/port/patches/vitagl-sh3.patch
make -B NO_DEBUG=1 STORE_DEPTH_STENCIL=1 -j"$(nproc)"
```

Use `make -B` whenever you change these flags: make does not track them. See [docs/WORKFLOW.md](docs/WORKFLOW.md) for details.

### 5. Build the VPK

```sh
SH3_VITAGL_DIR=~/src/vitaGL bash port/build.sh
```

The result is `build/sh3vita.vpk` (the build itself runs in `~/build/sh3vita`, because compiling on a Windows drive from WSL is slow).

### 6. Build the preparer

```sh
cd tools/sh3prep
cargo test --release
cargo build --release
```

On Linux, the GUI needs `libgtk-3-dev libxkbcommon-dev libwayland-dev`. Prebuilt binaries for Windows, macOS and Linux are produced by the GitHub Actions workflow `sh3prep`.

### Testing

- Recompiler checks: the `recomp/test_*.py` scripts.
- Console adaptations: every `tools/*_unit.py` refuses a generated function whose SHA-256 differs from the one it was written for.
- Build options, console tooling and the developer workflow: [docs/WORKFLOW.md](docs/WORKFLOW.md). Prepared data layout: [docs/PREPARED-DATA.md](docs/PREPARED-DATA.md).

### Layout

| Folder | Content |
|:--|:--|
| `recomp/` | x86 to C recompiler and its tests |
| `port/` | Runtime: Win32, D3D8, DirectX replacements, Vita layer |
| `port/patches/` | Patch for vitaGL |
| `port/livearea/` | LiveArea artwork |
| `tools/` | Console adaptations of generated functions |
| `tools/sh3prep/` | Desktop preparer (Rust) |
| `re/` | Ghidra scripts |
| `docs/` | Documentation |

## License

The original code in this repository (the recompiler, the runtime, the Vita layer and the `sh3prep` preparer) is released under the **GNU General Public License v3.0**. See [LICENSE](LICENSE).

Third-party components keep their own licenses. Their texts are in [LICENSES/](LICENSES/) and the list is in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The GPL covers this project's code only. It does not cover Silent Hill 3, and it does not cover artwork that comes with its own terms (see Credits).

## Legal

**Silent Hill 3** belongs to Konami. This project is not affiliated with, endorsed by or sponsored by Konami or Sony.

- No game file is included in this repository, in the VPK or in the preparer. The port only works with your own legitimate copy of the game, which `sh3prep` checks by hash before preparing anything.
- The C code generated from the game executable is created on your own machine and is never distributed.
- Do not ask for game files, ROMs, dumps or pirated copies in issues or discussions. They will be removed.
- You need a Vita with custom firmware. Install and use this port at your own risk.

## Credits

**Project**
- Port, recompiler, runtime and tools: [vallfrr](https://github.com/vallfrr).
- Testing and feedback: the testers who played the game and sent their logfiles.
- AI tools assisted the reverse engineering and the coding (Claude and Codex). Design decisions and all validation on real hardware were done by hand.

**Vita ecosystem**
- [VitaSDK](https://vitasdk.org) and vita-headers.
- [vitaGL](https://github.com/Rinnegatamante/vitaGL), vitaShaRK, SceShaccCgExt and math-neon, by Rinnegatamante and contributors.
- [ShaRKF00D](https://github.com/Rinnegatamante/ShaRKF00D), which installs the shader compiler module the game needs.
- [taiHEN](https://github.com/yifanlu/taiHEN) and [HENkaku](https://henkaku.xyz).
- [VitaShell](https://github.com/TheOfficialFloW/VitaShell), which the install steps rely on.

**Libraries**
- [SDL2](https://www.libsdl.org) (zlib license).
- [pl_mpeg](https://github.com/phoboslab/pl_mpeg) by Dominic Szablewski (MIT).
- [TLSF](https://github.com/mattconte/tlsf) by Matthew Conte (BSD 3-clause).
- [FFmpeg](https://ffmpeg.org) (GPL build from [ffmpeg-static](https://github.com/eugeneware/ffmpeg-static)), embedded in `sh3prep` for the movie conversion.

**Reverse engineering tools**
- [Ghidra](https://ghidra-sre.org)
- [Capstone](https://www.capstone-engine.org).

**Artwork**
- LiveArea: community images from [SteamGridDB](https://www.steamgriddb.com) (icon by FrostWolf, hero by Maxine, logo by kaito); Silent Hill and its logo belong to Konami.
- Controller screen: Vita outline by Tokyoship (Wikimedia Commons, CC BY 3.0), recolored; button prompts from [Kenney Input Prompts](https://kenney.nl/assets/input-prompts) (CC0).
