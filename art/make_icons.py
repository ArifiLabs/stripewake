"""Build the tray icons in art/ from head images on a transparent background. The .rc file never changes.

    python art/make_icons.py                       # on + off from art/stripewake-head-awake.png / -off.png
    python art/make_icons.py --timer art/stripewake-head-timer.png   # timer icon only

Each slot is rebuilt only from its own image, so swapping one icon is one file and one command,
then build.cmd. Strip AI metadata from new images first (python -m arifi_core.marketing.strip_ai_metadata).
PIL output carries no C2PA.
"""
import argparse
from pathlib import Path

from PIL import Image, ImageDraw

ART = Path(__file__).resolve().parent
SIZES = [16, 20, 24, 28, 32, 36, 40, 48, 256]  # tray sizes for 100-300% scaling, plus Explorer 256


def main_shape(im: Image.Image) -> Image.Image:
    """Clear stray specks: keep only pixels connected (through any non-transparent pixel) to the main shape."""
    alpha = im.getchannel("A")
    mask = alpha.point(lambda v: 255 if v else 0)
    box = mask.getbbox()
    seed = ((box[0] + box[2]) // 2, (box[1] + box[3]) // 2)
    if mask.getpixel(seed) != 255:
        raise SystemExit(f"center {seed} is transparent; cannot find the main shape")
    ImageDraw.floodfill(mask, seed, 128)
    keep = mask.point(lambda v: 255 if v == 128 else 0)
    dropped = sum(alpha.point(lambda v: 1 if v else 0).histogram()[1:]) - keep.histogram()[255]
    print(f"  dropped {dropped} stray pixel(s) outside the main shape")
    out = im.copy()
    out.putalpha(Image.composite(alpha, Image.new("L", im.size, 0), keep))
    return out


def square(path: Path) -> Image.Image:
    im = main_shape(Image.open(path).convert("RGBA"))
    im = im.crop(im.getchannel("A").getbbox() or (0, 0, im.width, im.height))
    side = max(im.size)
    out = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    out.paste(im, ((side - im.width) // 2, (side - im.height) // 2))
    return out.resize((256, 256), Image.LANCZOS)


def write(slot: str, src: Path) -> None:
    img = square(src)
    img.save(ART / f"stripewake-{slot}.ico", sizes=[(s, s) for s in SIZES])
    img.save(ART / f"stripewake-{slot}-preview.png")
    print(f"wrote stripewake-{slot}.ico from {src.name}")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--on", type=Path, help="awake head (default art/stripewake-head-awake.png)")
    ap.add_argument("--off", type=Path, help="asleep head (default art/stripewake-head-off.png)")
    ap.add_argument("--timer", type=Path, help="awake head with a stopwatch badge; only this slot is rebuilt")
    a = ap.parse_args()
    if a.timer:
        write("timer", a.timer)
        return
    write("on", a.on or ART / "stripewake-head-awake.png")
    write("off", a.off or ART / "stripewake-head-off.png")


if __name__ == "__main__":
    main()
