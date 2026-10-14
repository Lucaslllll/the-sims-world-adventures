#!/usr/bin/env python3
"""Render original launcher vectors to legacy PNGs and adaptive-mask previews.

The globe/compass/aircraft artwork is authored for this port. Rendering follows
the vector-to-PNG workflow used by the Ambitions reference project.
"""
from pathlib import Path
import re
import xml.etree.ElementTree as ET

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
RES = ROOT / "android/app/src/main/res"
ANDROID = "{http://schemas.android.com/apk/res/android}"
SCALE = 8
SIZE = 108 * SCALE


def contours(data):
    tokens = re.findall(r"[MLCZ]|-?\d+(?:\.\d+)?", data)
    result, points = [], []
    cursor, command = 0, None
    while cursor < len(tokens):
        if tokens[cursor] in "MLCZ":
            command = tokens[cursor]
            cursor += 1
            if command == "M" and points:
                result.append((points, False))
                points = []
        if command == "Z":
            result.append((points, True))
            points, command = [], None
            continue
        if command in ("M", "L"):
            points.append(tuple(float(v) for v in tokens[cursor:cursor + 2]))
            cursor += 2
            command = "L"
        elif command == "C":
            p0 = points[-1]
            values = [float(v) for v in tokens[cursor:cursor + 6]]
            cursor += 6
            p1, p2, p3 = values[:2], values[2:4], values[4:]
            for i in range(1, 33):
                t, u = i / 32, 1 - i / 32
                points.append(tuple(u**3 * p0[k] + 3 * u*u*t * p1[k]
                                    + 3 * u*t*t * p2[k] + t**3 * p3[k] for k in range(2)))
        else:
            raise ValueError(f"Unsupported path command: {command}")
    if points:
        result.append((points, False))
    return result


def rgb(value):
    value = value.lstrip("#")
    return tuple(int(value[i:i + 2], 16) for i in (0, 2, 4))


def paths(node, sx=1, sy=1, tx=0, ty=0):
    if node.tag == "group":
        gx = float(node.get(ANDROID + "scaleX", "1"))
        gy = float(node.get(ANDROID + "scaleY", "1"))
        tx += sx * float(node.get(ANDROID + "pivotX", "0")) * (1 - gx)
        ty += sy * float(node.get(ANDROID + "pivotY", "0")) * (1 - gy)
        sx, sy = sx * gx, sy * gy
    if node.tag == "path":
        yield node, sx, sy, tx, ty
    else:
        for child in node:
            yield from paths(child, sx, sy, tx, ty)


def render(path):
    image = Image.new("RGBA", (SIZE, SIZE))
    for node, sx, sy, tx, ty in paths(ET.parse(path).getroot()):
        shapes = [([(int(round((x * sx + tx) * SCALE)), int(round((y * sy + ty) * SCALE)))
                    for x, y in points], closed) for points, closed in contours(node.get(ANDROID + "pathData"))]
        gradient = next((g for g in node.iter() if g.tag == "gradient"), None)
        color = node.get(ANDROID + "fillColor")
        if color or gradient is not None:
            mask = Image.new("L", (SIZE, SIZE))
            draw = ImageDraw.Draw(mask)
            opacity = round(255 * float(node.get(ANDROID + "fillAlpha", "1")))
            for points, _ in shapes:
                draw.polygon(points, fill=opacity)
            if gradient is None:
                layer = Image.new("RGBA", (SIZE, SIZE), rgb(color) + (255,))
            else:
                start, end = rgb(gradient.get(ANDROID + "startColor")), rgb(gradient.get(ANDROID + "endColor"))
                layer = Image.new("RGBA", (SIZE, SIZE))
                fill = ImageDraw.Draw(layer)
                for y in range(SIZE):
                    t = y / (SIZE - 1)
                    fill.line([(0, y), (SIZE, y)], fill=tuple(round(a * (1 - t) + b * t)
                                                           for a, b in zip(start, end)) + (255,))
            layer.putalpha(mask)
            image = Image.alpha_composite(image, layer)
        stroke = node.get(ANDROID + "strokeColor")
        if stroke:
            layer = Image.new("RGBA", (SIZE, SIZE))
            draw = ImageDraw.Draw(layer)
            width = max(1, round(float(node.get(ANDROID + "strokeWidth", "1")) * sx * SCALE))
            opacity = round(255 * float(node.get(ANDROID + "strokeAlpha", "1")))
            for points, closed in shapes:
                draw.line(points + ([points[0]] if closed else []), fill=rgb(stroke) + (opacity,), width=width, joint="curve")
            image = Image.alpha_composite(image, layer)
    return image


def masked(image, circle=False):
    image = image.copy()
    mask = Image.new("L", image.size)
    draw = ImageDraw.Draw(mask)
    bounds = (0, 0, image.width - 1, image.height - 1)
    if circle:
        draw.ellipse(bounds, fill=255)
    else:
        draw.rounded_rectangle(bounds, radius=14 * SCALE, fill=255)
    image.putalpha(mask)
    return image


def main():
    image = Image.alpha_composite(render(RES / "drawable/ic_launcher_background.xml"),
                                  render(RES / "drawable/ic_launcher_foreground.xml"))
    # Adaptive icons display the middle 72dp of their 108dp layers.
    image = image.crop(tuple(v * SCALE for v in (18, 18, 90, 90)))
    legacy = masked(image)
    for density, size in {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}.items():
        legacy.resize((size, size), Image.Resampling.LANCZOS).save(RES / f"mipmap-{density}/ic_launcher.png")
    previews = ROOT / "docs/img"
    legacy.resize((512, 512), Image.Resampling.LANCZOS).save(previews / "icone-world-adventures.png")
    sheet = Image.new("RGB", (760, 340), "#182B41")
    draw = ImageDraw.Draw(sheet)
    for column, circle in enumerate((False, True)):
        icon = masked(image, circle)
        draw.text((20 + column * 380, 12), "Rounded square" if not circle else "Adaptive circle", fill="white")
        for size, x, y in ((192, 20, 44), (96, 230, 64), (48, 250, 194)):
            resized = icon.resize((size, size), Image.Resampling.LANCZOS)
            sheet.paste(resized, (x + column * 380, y), resized)
            draw.text((x + column * 380, y + size + 8), f"{size}px", fill="#C7DDEE")
    sheet.save(previews / "icone-world-adventures-mascaras.png")
    print(f"Launcher resources updated; preview: {previews / 'icone-world-adventures.png'}")


if __name__ == "__main__":
    main()
