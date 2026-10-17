import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path("C:/ArifiLabs/tools/stripewake")
OUT = Path(sys.argv[1])
W, H = 1920, 1080
BG, FG, SUB, ACC = (20, 22, 26), (255, 255, 255), (178, 184, 194), (255, 170, 40)
F = lambda s, b=True: ImageFont.truetype("C:/Windows/Fonts/segoeuib.ttf" if b else "C:/Windows/Fonts/segoeui.ttf", s)


def fit(img, mw, mh, resample=Image.LANCZOS):
    r = min(mw / img.width, mh / img.height)
    return img.resize((max(1, int(img.width * r)), max(1, int(img.height * r))), resample)


def center_text(d, y, text, f, fill):
    w = d.textlength(text, font=f)
    d.text(((W - w) / 2, y), text, font=f, fill=fill)


def poster(src, name, title, sub):
    c = Image.new("RGB", (W, H), BG); d = ImageDraw.Draw(c)
    im = fit(Image.open(src).convert("RGBA"), 1700, 800)
    c.paste(im, ((W - im.width) // 2, 60), im)
    center_text(d, 885, title, F(64), FG); center_text(d, 975, sub, F(34, False), SUB)
    c.save(OUT / name)


poster(ROOT / "art/stripewake-hero.webp", "screenshot-4-hero-1920x1080.png",
       "Stripewake keeps your PC awake.", "Local AI runs, builds, renders, downloads, demos: nothing stops halfway.")
poster(ROOT / "art/stripewake-banner.png", "screenshot-5-no-plastic-clip-1920x1080.png",
       "No plastic clip. No mouse jiggler.", "One tray icon keeps the PC and the screen awake until you say stop.")

c = Image.new("RGB", (W, H), BG); d = ImageDraw.Draw(c)
center_text(d, 80, "Weighs almost nothing.", F(80), FG)
center_text(d, 200, "Measured on one PC, same 10-minute idle window.", F(34, False), SUB)
rows = [("", "Stripewake", "PowerToys (all)"), ("Processes", "1", "4"), ("Private memory", "1.8 MB", "140.7 MB"),
        ("CPU over 10 min idle", "0 ms", "1,234 ms"), ("Install size", "0.47 MB", "1,033 MB")]
xs = (260, 1010, 1430)
for i, row in enumerate(rows):
    y = 330 + i * 130
    for j, cell in enumerate(row):
        col = SUB if i == 0 else (ACC if j == 1 else FG)
        d.text((xs[j], y), cell, font=F(56 if i else 44, j != 2 or i == 0), fill=col)
    if i:
        d.line((260, y - 22, 1700, y - 22), fill=(60, 64, 72), width=2)
c.save(OUT / "screenshot-6-vs-powertoys-1920x1080.png")

c = Image.new("RGB", (W, H), BG); d = ImageDraw.Draw(c)
center_text(d, 120, "The tray icon tells you the state.", F(72), FG)
labels = [("stripewake-head-awake.png", "ON"), ("stripewake-head-timer.png", "Timer running"), ("stripewake-head-off.png", "OFF")]
for i, (f, lab) in enumerate(labels):
    im = fit(Image.open(ROOT / "art" / f).convert("RGBA"), 360, 360)
    x = 260 + i * 540
    c.paste(im, (x + (360 - im.width) // 2, 330 + (360 - im.height) // 2), im)
    w = d.textlength(lab, font=F(52)); d.text((x + (360 - w) / 2, 740), lab, font=F(52), fill=ACC if i == 0 else FG)
center_text(d, 900, "Hover for a live countdown: \"ON - 1 h 23 m left (until 17:30)\"", F(36, False), SUB)
c.save(OUT / "screenshot-7-tray-icon-states-1920x1080.png")
print("ok")
