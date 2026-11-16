# Known issues

State of the PS Vita build, version 0.1.0 (November 2026). "Console" means seen on a real PS Vita.

## Open

| Issue | Notes |
|---|---|
| Flashlight glow and lens flares missing | **Not resolved, purely cosmetic.** The soft glow around Heather and the halos around lamps (visible on the PC) are not rendered; the scene stays readable and playable, and there is no black square. It no longer costs any frame rate: the game's lens-visibility probe (a render-target switch and GPU readback every frame) is bypassed on purpose. The missing glow is probably the game's bloom pass (a blurred 256×256 copy of the image added back to the scene), which produces nothing on the Vita; the cause is still being investigated. |
| Frame rate depends on the area | The game adapts its logic step to the frame time: 60, 30 or 20 Hz. Measured on the console: about 38 fps in the lightest rooms, 25–30 fps in most areas, 17–21 fps in the church, hospital back rooms, parts of the amusement park and the sewers. |
| Memory is tight in very long sessions | The guest heap fragments over many zone changes; allocation failures were seen after dozens of consecutive zone changes. Save regularly. |
| First launch stutters | Shaders are compiled on the console the first time they are needed. A compiled-shader cache shipped in the VPK covers many of them, and what is compiled is kept in `ux0:data/sh3/cache/`. |
| Jagged edges | No anti-aliasing. |
| Some options are PC-only | Resolution and display-mode rows are hidden; the single Vita resolution is used. |
| Most of the game | Only the beginning of the game and a sample of the zones were played. Later areas, endings and long sessions are unconfirmed. |

## Confirmed on the console

- Boot, title menu, options (including gamma), new game, saving and loading.
- The seven movies at 30 fps with the converted MP4 files.
- Inventory with the pad, pause menu, fades and loading screens.
- Sound effects, music and voices.

## Things to know

- **Executable:** only the `sh3.exe` listed in the README works: the recompiled code depends on it byte for byte.
- **Logs:** `ux0:data/sh3/diag/diag.log`. Include it when reporting a problem.
- **Updating:** install the new VPK over the old one; keep `ux0:data/sh3/`.
