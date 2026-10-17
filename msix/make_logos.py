"""Write the MSIX logo set (Start, taskbar, Store) from the awake head art into <layout>/Assets.

    python msix/make_logos.py build/msix

Run by msix/build-msix.cmd; the PNGs are build output, never committed.
"""
import sys
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "art"))
from make_icons import main_shape  # noqa: E402  (same speck clean-up as the tray icons)

HEAD = Path(__file__).resolve().parents[1] / "art" / "stripewake-head-awake.png"


def head() -> Image.Image:
    im = main_shape(Image.open(HEAD).convert("RGBA"))
    return im.crop(im.getchannel("A").getbbox())


def logo(src: Image.Image, w: int, h: int, fill: float) -> Image.Image:
    """src centered on a transparent w x h canvas, its longer side = fill x the canvas height."""
    s = min(fill * h / src.height, fill * h / src.width)
    art = src.resize((max(1, round(src.width * s)), max(1, round(src.height * s))), Image.LANCZOS)
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    out.paste(art, ((w - art.width) // 2, (h - art.height) // 2), art)
    return out


def main() -> None:
    assets = Path(sys.argv[1]) / "Assets"
    assets.mkdir(parents=True, exist_ok=True)
    src = head()
    jobs = {}
    for scale in (100, 125, 150, 200, 400):
        k = scale / 100
        jobs[f"Square44x44Logo.scale-{scale}.png"] = (round(44 * k), round(44 * k), 1.0)
        jobs[f"Square150x150Logo.scale-{scale}.png"] = (round(150 * k), round(150 * k), 0.66)
        jobs[f"Wide310x150Logo.scale-{scale}.png"] = (round(310 * k), round(150 * k), 0.66)
        jobs[f"StoreLogo.scale-{scale}.png"] = (round(50 * k), round(50 * k), 1.0)
    for t in (16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 80, 96, 256):  # taskbar, Start list, Alt+Tab
        jobs[f"Square44x44Logo.targetsize-{t}.png"] = (t, t, 1.0)
        jobs[f"Square44x44Logo.targetsize-{t}_altform-unplated.png"] = (t, t, 1.0)
        jobs[f"Square44x44Logo.targetsize-{t}_altform-lightunplated.png"] = (t, t, 1.0)
    for name, (w, h, fill) in jobs.items():
        logo(src, w, h, fill).save(assets / name)
    print(f"wrote {len(jobs)} logos to {assets}")


if __name__ == "__main__":
    main()
