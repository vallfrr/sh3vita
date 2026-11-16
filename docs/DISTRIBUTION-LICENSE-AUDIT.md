# Distribution and license notes

Status of what is published and why. This is a working audit by the maintainer, not legal advice.

## What is published

| Item | In the repository | In the VPK | In the preparer |
|---|---|---|---|
| Recompiler, runtime, Vita layer, preparer (GPL-3.0) | yes | compiled | yes |
| Code generated from `sh3.exe` (`port/gen/`) | **never** | compiled into the executable | no |
| Game executable, data, movies, sounds | **never** | **never** | **never** |
| GPU / D3DX shader caches harvested on a console | **never** | yes (compiled records) | no |
| vitaGL | patch only (`port/patches/vitagl-sh3.patch`) | linked | no |

The same arrangement as [sh4vita](https://github.com/vallfrr/sh4vita): the game's code is translated on the user's own
machine for source builds, and the released VPK is the compiled result of that translation of one specific executable.
The VPK works only together with the user's own copy of the game (the executable is loaded from `ux0:data/sh3/` and its hash
is checked by `sh3prep` before preparation).

## Open points

- The VPK contains machine code derived from the game executable. Whether and how that may be distributed is the maintainer's
  decision and risk; if a rights holder objects, the VPK is withdrawn and only the sources and tools remain.
- The supported `sh3.exe` is an unprotected build of the PC executable. This project does not distribute it and does not explain
  how to obtain it.
- SceShaccCgExt is GPL-3.0; the combined executable is distributed under GPL-3.0 terms with corresponding sources in this repository.
- Third-party license texts: `LICENSES/`. Notices: `THIRD_PARTY_NOTICES.md`. The collection is not a complete dependency certification.

- The LiveArea images (logo, portrait, background) come from SteamGridDB community uploads of Silent Hill 3 artwork. The logo and imagery are
  Konami's; they are included for identification, are credited, and would be removed or replaced if requested.

## Checks done before publishing

`tools/release/make_release_tree.py` (kept in the private development tree) builds this repository from a whitelist and scans it for
game file types, large files, private paths and development leftovers before anything is published.
