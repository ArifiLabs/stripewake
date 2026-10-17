import sys
from pathlib import Path
from PIL import Image

ROOT = Path("C:/ArifiLabs/tools/stripewake")
OUT = Path(sys.argv[1])
BG = (20, 22, 26)


def fit(img, mw, mh):
    r = min(mw / img.width, mh / img.height)
    return img.resize((int(img.width * r), int(img.height * r)), Image.LANCZOS)


hero = Image.open(ROOT / "art/stripewake-hero.webp").convert("RGBA")
c = Image.new("RGB", (1440, 2160), BG)
im = fit(hero, 1440, 2160)
c.paste(im, ((1440 - im.width) // 2, (2160 - im.height) // 2), im)
c.save(OUT / "poster-art-9x16-1440x2160.png")

head = Image.open(ROOT / "art/stripewake-head-awake.png").convert("RGBA")
for size, name in ((2160, "box-art-1x1-2160x2160.png"),):
    c = Image.new("RGB", (size, size), BG)
    im = fit(head, int(size * 0.82), int(size * 0.82))
    c.paste(im, ((size - im.width) // 2, (size - im.height) // 2), im)
    c.save(OUT / name)

tile = Image.open(OUT / "app-tile-icon-300x300.png").convert("RGBA")
for s in (150, 71):
    tile.resize((s, s), Image.LANCZOS).save(OUT / f"app-icon-{s}x{s}.png")
print("ok")
