# LiveArea artwork

| File | Use | Source |
|---|---|---|
| `icon0.png` (128×128) | LiveArea bubble icon | Icon for Silent Hill 3 by FrostWolf, [SteamGridDB #2856](https://www.steamgriddb.com/icon/2856), resized |
| `bg.png` (840×500) | LiveArea background behind the button | Hero for Silent Hill 3 by Maxine, [SteamGridDB #64119](https://www.steamgriddb.com/hero/64119), cropped and resized |
| `startup.png` (280×158) | The LiveArea start button: the logo alone, transparent background (the background image shows through) | Logo for Silent Hill 3 by kaito, [SteamGridDB #5447](https://www.steamgriddb.com/logo/5447) |
| `pic0.png` (960×544) | Launch screen: the same logo on black | same as above |
| `../../docs/logo.png`, `logo-dark.png` | README banner: the logo with transparent background (white, and an inverted copy for light themes) | same as above |
| `template.xml` | LiveArea layout | this project |

The images are community artwork about Silent Hill 3 gathered from SteamGridDB; the logo and the game's imagery belong to
Konami. They are used here for identification only and are not covered by this project's GPL license.
The Vita only accepts 8-bit indexed PNG files inside a VPK (full-colour PNGs fail with install error 0x8010113D); they were
converted with ImageMagick, for example:

```sh
magick icon.png -resize 128x128 -colors 256 -dither FloydSteinberg PNG8:icon0.png
magick hero.png -resize x500 -gravity center -crop 840x500+0+0 +repage -colors 256 -dither FloydSteinberg PNG8:bg.png
# startup.png: magick cannot write a PNG8 with a partial alpha palette; resize the white logo to 252x, centre it on a transparent 280x158 canvas,
# then write an 8-bit palette PNG (white entries + tRNS, 33 alpha levels) with a short Python script (zlib + struct).
magick logo.png -resize 760x logo-big.png && magick -size 960x544 xc:black logo-big.png -gravity center -composite -colors 64 PNG8:pic0.png
```
