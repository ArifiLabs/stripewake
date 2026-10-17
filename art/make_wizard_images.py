"""Installer wizard art from the mascot files: Welcome/Finish side panel (poster) and top-right corner (tiger head).
Inno Setup picks the size that fits the screen scaling, so every scaling step gets its own 24-bit BMP."""
from pathlib import Path

from PIL import Image

ART = Path(__file__).parent
OUT = ART / "wizard"
SIDE = [(164, 314), (192, 386), (246, 459), (273, 520), (328, 628), (355, 666), (410, 797)]  # 100% .. 250%
SMALL = [55, 64, 69, 83, 92, 110, 119, 138]


def contain(img: Image.Image, w: int, h: int) -> Image.Image:
    """The whole poster (name included), scaled to the panel width, centred on the poster's black."""
    s = min(w / img.width, h / img.height)
    r = img.resize((round(img.width * s), round(img.height * s)), Image.LANCZOS)
    out = Image.new("RGB", (w, h), (0, 0, 0))
    out.paste(r, ((w - r.width) // 2, (h - r.height) // 2))
    return out


def main() -> None:
    OUT.mkdir(exist_ok=True)
    poster = Image.open(ART / "stripewake-mascot-poster.png").convert("RGB")
    for w, h in SIDE:
        contain(poster, w, h).save(OUT / f"side-{w}x{h}.bmp")
    head = Image.open(ART / "stripewake-head-awake.png").convert("RGBA")
    for n in SMALL:
        bg = Image.new("RGBA", (n, n), (255, 255, 255, 255))
        bg.alpha_composite(head.resize((n, n), Image.LANCZOS))
        bg.convert("RGB").save(OUT / f"corner-{n}.bmp")
    names = sorted(p.name for p in OUT.glob("*.bmp"))
    assert len(names) == len(SIDE) + len(SMALL), names
    print("\n".join(names))


if __name__ == "__main__":
    main()
