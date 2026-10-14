#!/usr/bin/env python3
"""Archive curated HD validation evidence and side-by-side comparisons."""
import json
from pathlib import Path
import shutil
from PIL import Image, ImageDraw
from upscale_graphics import compare_sheet, SAMPLES

ROOT = Path(__file__).resolve().parents[1]
graphics = ROOT / "analysis/graphics"
manifest = json.loads((graphics / "manifest.json").read_text())
selected = [next(e for e in manifest["entries"] if name in e["source"]) for name in SAMPLES]
compare_sheet(manifest, graphics, graphics / "hdpack", selected)
dest = ROOT / "docs/img/hd"
dest.mkdir(parents=True, exist_ok=True)
for source, name in ((graphics / "upscale-comparison.png", "texturas-comparacao.png"),
                     (graphics / "device-original/world.png", "mundo-original.png"),
                     (graphics / "device-hd/pause.png", "mundo-hd.png"),
                     (graphics / "device-hd/boot-20261013-164113.png", "menu-hd.png")):
    if not (dest / name).exists():
        shutil.copy2(source, dest / name)
original = Image.open(dest / "mundo-original.png").convert("RGB")
hd = Image.open(dest / "mundo-hd.png").convert("RGB")
# Match the same captured world region, avoiding clock/player animation differences.
crop = (315, 55, 720, 450)
sheet = Image.new("RGB", (1220, 640), "#253146")
draw = ImageDraw.Draw(sheet)
for column, image in enumerate((original, hd)):
    image = image.crop(crop).resize((600, 585), Image.Resampling.NEAREST)
    sheet.paste(image, (column * 610 + 5, 45))
draw.text((10, 15), "Antes — render 480x320 + texturas originais", fill="white")
draw.text((620, 15), "Depois — render 960x640 + Real-ESRGAN 2x", fill="white")
sheet.save(dest / "jogo-comparacao.png")
print(f"HD evidence and comparisons: {dest}")
