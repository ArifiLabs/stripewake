"""Store listing images: 1920x1080 desktop screenshots (Store minimum 1366x768) from docs/screenshots, plus the
300x300 app tile icon.

    python msix/make_store_assets.py <out dir>
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from make_logos import head, logo

ROOT = Path(__file__).resolve().parents[1]
SHOTS = ROOT / "docs" / "screenshots"
BG, FG, SUB = (20, 22, 26), (255, 255, 255), (178, 184, 194)
W, H = 1920, 1080
# (screenshot, caption, subcaption)
SLIDES = [
    ("6-display-submenu.png", "Screen on. Dims itself when you walk away.",
     "Every screen drops to 30, 50 or 70% after 2 to 15 minutes; one touch brings it back."),
    ("2-keep-awake-submenu.png", "One click keeps your PC awake.",
     "Indefinitely, 30 minutes to 8 hours, or until a date and time you pick."),
    ("3-status-check.png", "Know what can still interrupt you.",
     "Every lock, blank-screen and restart trap, worst first, with a button to the exact setting."),
    ("1-tray-menu.png", "Everything from one tray icon.",
     "499 KB, 1.8 MB of memory, 0 ms of CPU while idle."),
]


def font(size: int, bold: bool) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(r"C:\Windows\Fonts\segoeuib.ttf" if bold else r"C:\Windows\Fonts\segoeui.ttf", size)


def slide(shot: Path, title: str, sub: str) -> Image.Image:
    out = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(out)
    mark = logo(head(), 96, 96, 1.0)
    out.paste(mark, (80, 56), mark)
    d.text((200, 62), title, font=font(52, True), fill=FG)
    d.text((202, 132), sub, font=font(28, False), fill=SUB)
    im = Image.open(shot).convert("RGBA")
    s = min(1640 / im.width, 790 / im.height, 2.0)
    im = im.resize((round(im.width * s), round(im.height * s)), Image.LANCZOS)
    x, y = (W - im.width) // 2, 220 + (820 - im.height) // 2
    shadow = Image.new("RGBA", (im.width + 24, im.height + 24), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle((0, 0, shadow.width - 1, shadow.height - 1), 18, fill=(0, 0, 0, 110))
    out.paste(shadow, (x - 12, y - 4), shadow)
    out.paste(im, (x, y), im)
    return out


def main() -> None:
    dst = Path(sys.argv[1])
    dst.mkdir(parents=True, exist_ok=True)
    for i, (name, title, sub) in enumerate(SLIDES, 1):
        p = dst / f"screenshot-{i}-{Path(name).stem.split('-', 1)[1]}-1920x1080.png"
        slide(SHOTS / name, title, sub).save(p)
        print("wrote", p.name)
    logo(head(), 300, 300, 0.9).save(dst / "app-tile-icon-300x300.png")
    print("wrote app-tile-icon-300x300.png")


if __name__ == "__main__":
    main()
