# LiveArea artwork

| File | Use | Source |
|---|---|---|
| `icon0.png` (128×128) | LiveArea bubble icon | Icon for Silent Hill 3 by FrostWolf, [SteamGridDB #2856](https://www.steamgriddb.com/icon/2856), resized |
| `bg.png` (840×500) | LiveArea background behind the button | Hero for Silent Hill 3 by Maxine, [SteamGridDB #64119](https://www.steamgriddb.com/hero/64119), cropped and resized |
| `startup.png` (280×158) | The LiveArea start button | Logo for Silent Hill 3 by kaito, [SteamGridDB #5447](https://www.steamgriddb.com/logo/5447), on black |
| `pic0.png` (960×544) | Launch screen: the same logo on black | same as above |
| `template.xml` | LiveArea layout | this project |

The images are community artwork about Silent Hill 3 gathered from SteamGridDB; the logo and the game's imagery belong to
Konami. They are used here for identification only and are not covered by this project's GPL license.
The Vita only accepts 8-bit indexed PNG files inside a VPK (full-colour PNGs fail with install error 0x8010113D); they were
converted with ImageMagick, for example:

```sh
magick icon.png -resize 128x128 -colors 256 -dither FloydSteinberg PNG8:icon0.png
magick hero.png -resize x500 -gravity center -crop 840x500+0+0 +repage -colors 256 -dither FloydSteinberg PNG8:bg.png
magick logo.png -resize 252x logo-small.png && magick -size 280x158 xc:black logo-small.png -gravity center -composite -colors 32 PNG8:startup.png
magick logo.png -resize 760x logo-big.png && magick -size 960x544 xc:black logo-big.png -gravity center -composite -colors 64 PNG8:pic0.png
```
