# Controller artwork

Vita outline: Tokyoship, Wikimedia Commons, CC BY 3.0.
https://commons.wikimedia.org/wiki/File:PSV_Layout.svg
https://creativecommons.org/licenses/by/3.0/
Adapted by recoloring black strokes white, retaining transparency.

All button prompts: Kenney Input Prompts, CC0 1.0. See Kenney-License.txt.
https://kenney.nl/assets/input-prompts
https://github.com/Maaack/Kenney-Input-Prompts/tree/1.3
Original PlayStation outline SVGs and Switch L/R outline SVGs, unchanged.

controls.tga packs this existing artwork for the runtime. Rebuild using
node tools/build_controls_assets.cjs (sharp required). Sprite IDs are exposed
in port/src/d3d8/console_ui.h for reuse in other native menus.
No game files in this artwork. Included in the VPK, no data preparation needed.

SH3 adaptation: artwork copied byte-for-byte from the completed SH4 port on
2026-11-12 (SH4 remains read-only). Native SH4 code addresses and gameplay
actions are not reusable unchanged. SH3 runtime labels/mapping must be qualified
independently. This atlas contains no commercial game assets.
