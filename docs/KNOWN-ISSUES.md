# Known issues

State of the PS Vita build, version 1.0.3 (November 2026). "Console" means seen on a real PS Vita.

## Open

| Issue | Notes |
|---|---|
| Flashlight glow and lens flares missing | **Not resolved, purely cosmetic.** The soft glow around Heather and the halos around lamps (visible on the PC) are not rendered; the scene stays readable and playable, and there is no black square. It no longer costs any frame rate: the game's lens-visibility probe (a render-target switch and GPU readback every frame) is bypassed on purpose. The missing glow is probably the game's bloom pass (a blurred 256×256 copy of the image added back to the scene), which produces nothing on the Vita; the cause is still being investigated. |
| Shadows under characters missing | **Not resolved, cosmetic.** The game builds its shadow volumes but nothing visible is drawn from them on the Vita (the PC capture shows no shadow either in the scenes checked). They are switched off on purpose: with the flashlight on in the first zone they cost frame rate and show nothing. |
| Document and other picture screens | Screens drawn by the game's picture viewers (puzzles, documents, close-ups) should now fill the 16:9 screen like the map; only the book puzzle was checked on the console. Report any screen still shown in a 4:3 box with black bars. |
| Sleep mode can crash the game | Putting the Vita to sleep during play and waking it can close the game (a fault inside the GPU driver on the first frame after waking). Save before sleeping. |
| Frame rate depends on the area | Options > Frame rate: **60** (default) runs at fixed steps of 60, 30 or 20 images/s depending on the scene; **30** caps at 30 (30 or 20); **Unlimited** lets the rate vary but never above 60. Measured on the console (2026-11-17, 204 rooms from the school to the church, 12 s each, Unlimited): 124 rooms run at 54–60, 53 between 30 and 54, and 27 are under 30. The worst are in the hospital (floor 2 rooms, basement rooms, 15–25), the amusement park entrance (18) and one church room (25). Since then the hospital fog particles are drawn at reduced resolution like the other particles (they were left at full resolution by mistake): two hospital floor rooms gain 5 to 9 images/s; the worst room (hospital basement, about 15) is unchanged. |
| Heather jerks back and forth with "Unlimited" frame rate | With the Unlimited setting the game logic advances 1, 2 or 3 steps per image, so Heather's movement on screen is uneven (small backward/forward jumps). Not fixed; use the default **60** (or 30), which advance in even steps. |
| Memory is tight in very long sessions | The guest heap fragments over many zone changes. On the console all 266 rooms and the 7 movies pass in one session (2026-11-18, twice); since the texture copies are released after upload (2026-11-18), no allocation fails any more during that run (it was about 20 to 30, all recovered). Save regularly. |
| First launch stutters | Shaders are compiled on the console the first time they are needed. A compiled-shader cache shipped in the VPK covers many of them, and what is compiled is kept in `ux0:data/sh3/cache/`. |
| Jagged edges | No anti-aliasing. |
| Some options are PC-only | Resolution and display-mode rows are hidden; the single Vita resolution is used. |

## Confirmed on the console

- The game is playable from start to finish: a tester finished it on the console.
- Settings (brightness, frame rate, language) are kept between launches; saves and settings are written to a temporary file and renamed, so a crash during a save no longer leaves a truncated file.
- The map fills the 16:9 screen and shows Heather's position arrow; the book puzzle fills the screen, with the game's own pointer (left stick or D-pad, Cross clicks).
- The D-pad does the same as the left stick everywhere.
- Blood puddles stay fixed on the floor; the ceiling-light trails and the black trail at high brightness are gone.
- Bloody footprints stay on the floor when the camera moves and no longer blink (1.0.2).
- Options > Extra has a Movement row, 2D (default) or 3D, the game's own tank controls (1.0.2).
- The inventory screen has black sides in every room (1.0.2).
- The red Halo of the Sun seal is drawn on the save-point mirrors, including the first bathroom after the Douglas cutscene (1.0.3).
- All 266 rooms and the seven movies run in a single session (three full runs, 2026-11-18), with no allocation failure since the texture copies are released after upload.
- Boot, title menu, options (including gamma), new game, saving and loading.
- The seven movies at 30 fps with the converted MP4 files.
- Inventory with the pad, pause menu, fades and loading screens.
- Sound effects, music and voices.

## Things to know

- **Executable:** only the `sh3.exe` listed in the README works: the recompiled code depends on it byte for byte.
- **Logs:** `ux0:data/sh3/diag/diag.log` (current session) and `diag-1.log` ... `diag-10.log` (the 10 previous ones, `diag-1` is the latest). Include them when reporting a problem.
- **Updating:** install the new VPK over the old one; keep `ux0:data/sh3/`. The application ID is `SILHIL003` (earlier test versions used `SILH00003`: delete that old bubble, your saves stay in `ux0:data/sh3/`).
