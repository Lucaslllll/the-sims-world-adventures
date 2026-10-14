#!/usr/bin/env python3
"""Publish selected game-only UI evidence; never copy general device screenshots."""
import argparse
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]


def report(before, after, fit, stretch, settings):
    output = ROOT / "docs/img/ui"
    output.mkdir(parents=True, exist_ok=True)
    old, new = Image.open(before).convert("RGB"), Image.open(after).convert("RGB")
    if old.size != (1520, 720) or new.size != old.size:
        raise ValueError("Font crops require the validated 1520x720 menu captures")
    sheet = Image.new("RGB", (1120, 360), "#253146")
    draw = ImageDraw.Draw(sheet)
    draw.text((10, 10), "Antes: bitmap pequeno filtrado", fill="white")
    draw.text((570, 10), "Depois: glifos nativos 2x", fill="white")
    for row, box in enumerate(((480, 165, 740, 211), (480, 376, 740, 422), (480, 483, 740, 529))):
        for column, image in enumerate((old, new)):
            crop = image.crop(box).resize((520, 92), Image.Resampling.NEAREST)
            sheet.paste(crop, (10 + column * 560, 35 + row * 106))
    sheet.save(output / "fontes-comparacao.png")
    sheet = Image.new("RGB", (1440, 382), "#253146")
    draw = ImageDraw.Draw(sheet)
    for column, (path, label) in enumerate(((fit, "Proporcao original: fit"), (stretch, "Tela preenchida: stretch"))):
        image = Image.open(path).convert("RGB")
        draw.text((10 + column * 720, 10), label, fill="white")
        sheet.paste(image.resize((720, 341), Image.Resampling.LANCZOS), (column * 720, 35))
    sheet.save(output / "tela-comparacao.png")
    Image.open(settings).save(output / "opcoes-tela.png")
    Image.open(after).save(output / "menu-fonte-hd.png")
    print(f"Game UI evidence: {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("before", "after", "fit", "stretch", "settings"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    report(args.before, args.after, args.fit, args.stretch, args.settings)
