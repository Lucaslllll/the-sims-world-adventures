#!/usr/bin/env python3
"""Build a local 2x AI texture pack with original alpha and GL hash matching.

Requires extract_graphics.py and setup_upscaler.py. The pack is separate from
the APK and contains only derivatives of the player's own game resources.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
SAMPLES = ("texture_quad_hud_group02_cas_and_fe.m3g", "texture_objects.m3g", "texture_env_cas_room.m3g",
           "texture_townmap_ssv.m3g", "opaque_sm_naked_head_skin01.png", "paintover_sm_torso_adventurer_black.png")


def bleed_transparent_rgb(image, radius=6):
    pixels = np.array(image, dtype=np.uint8)
    rgb = pixels[:, :, :3].copy()
    known = pixels[:, :, 3] > 0
    height, width = known.shape
    for _ in range(radius):
        sums = np.zeros((height, width, 3), dtype=np.uint16)
        votes = np.zeros((height, width), dtype=np.uint8)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            target = (slice(max(0, dy), min(height, height + dy)), slice(max(0, dx), min(width, width + dx)))
            source = (slice(max(0, -dy), min(height, height - dy)), slice(max(0, -dx), min(width, width - dx)))
            mask = known[source]
            sums[target] += rgb[source].astype(np.uint16) * mask[:, :, None]
            votes[target] += mask
        fill = ~known & (votes > 0)
        if not bool(np.any(fill)):
            break
        rgb[fill] = (sums[fill] // votes[fill, None]).astype(np.uint8)
        known[fill] = True
    return Image.fromarray(rgb)


def compare_sheet(manifest, graphics, pack, selected):
    sheet = Image.new("RGB", (1120, len(selected) * 250), "#253146")
    draw = ImageDraw.Draw(sheet)
    for row, entry in enumerate(selected):
        original = Image.open(graphics / entry["original"]).convert("RGBA")
        hd = Image.open(pack / "images" / Path(entry["original"]).name).convert("RGBA")
        name = entry["source"].split("/")[-1].split("#")[0]
        # Identical crop positions and display size for a genuine comparison.
        side = min(128, original.width, original.height)
        left, top = 0, 0
        if "objects" in name:
            left, top = 256, 0
        elif "head_skin" in name:
            left, top = 32, 32
        elif "hud_group02" in name:
            left, top = 0, 64
        crop = (left, top, left + side, top + side)
        original = original.crop(crop)
        hd = hd.crop(tuple(value * 2 for value in crop))
        baseline = original.resize((220, 220), Image.Resampling.BILINEAR)
        upscale = hd.resize((220, 220), Image.Resampling.LANCZOS)
        for column, image in enumerate((baseline, upscale)):
            x, y = 120 + column * 560, row * 250 + 25
            sheet.paste(image, (x, y), image)
        draw.text((10, row * 250 + 5), "Original ampliado — " + name, fill="white")
        draw.text((570, row * 250 + 5), "Real-ESRGAN 2x + alpha original", fill="white")
    sheet.save(graphics / "upscale-comparison.png")


def upscale(graphics, pack, samples, tile):
    start = time.monotonic()
    manifest = json.loads((graphics / "manifest.json").read_text())
    tool_root = ROOT / "analysis/deps/realesrgan-ncnn-vulkan-20220424"
    report = json.loads((tool_root / "source.json").read_text())
    executable = ROOT / report["executable"]
    unique = {}
    selected = []
    for entry in manifest["entries"]:
        if samples and not any(name in entry["source"] for name in SAMPLES):
            continue
        unique.setdefault(Path(entry["original"]).name, entry)
        if any(name in entry["source"] for name in SAMPLES):
            selected.append(entry)
    pack.mkdir(parents=True, exist_ok=True)
    images = pack / "images"
    images.mkdir(exist_ok=True)
    inputs = graphics / ("ai-sample-input" if samples else "ai-input")
    outputs = graphics / ("ai-sample-output" if samples else "ai-output")
    inputs.mkdir(exist_ok=True)
    outputs.mkdir(exist_ok=True)
    pending = []
    for filename, entry in unique.items():
        destination = images / filename
        if destination.exists():
            with Image.open(destination) as image:
                if image.size != (entry["width"] * 2, entry["height"] * 2):
                    raise ValueError(f"Existing HD image has wrong dimensions: {destination}")
            continue
        original = Image.open(graphics / entry["original"]).convert("RGBA")
        bleed_transparent_rgb(original).save(inputs / filename)
        pending.append(filename)
    if pending:
        command = [str(executable), "-i", str(inputs), "-o", str(outputs), "-m", str(executable.parent / "models"),
                   "-n", "realesr-animevideov3", "-s", "2", "-t", str(tile), "-j", "1:1:1", "-f", "png"]
        print(f"AI upscale of {len(pending)} unique images; tile={tile}", flush=True)
        with (graphics / ("ai-sample.log" if samples else "ai-full.log")).open("w") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
        for filename in pending:
            entry = unique[filename]
            original = Image.open(graphics / entry["original"]).convert("RGBA")
            result = Image.open(outputs / filename).convert("RGB")
            expected = (entry["width"] * 2, entry["height"] * 2)
            if result.size != expected:
                raise ValueError(f"AI output dimension mismatch: {filename}")
            # The model only reconstructs RGB. Alpha comes exclusively from the original.
            result.putalpha(original.getchannel("A").resize(expected, Image.Resampling.LANCZOS))
            result.save(images / filename)
    rows = {}
    for entry in manifest["entries"]:
        filename = Path(entry["original"]).name
        if filename not in unique:
            continue
        for upload in entry["uploads"]:
            row = f'{upload["hash"]} {entry["width"]} {entry["height"]} {int(upload["premultiplied"])} images/{filename}'
            rows.setdefault(upload["hash"], row)
    (pack / "manifest.tsv").write_text("TS3WA_HD1\n" + "\n".join(rows[key] for key in sorted(rows)) + "\n")
    metadata = dict(scale=2, model="realesr-animevideov3-x2", upscaler=report,
                    unique_images=len(unique), texture_upload_keys=len(rows),
                    original_alpha_preserved=True, originals_modified=False,
                    samples_only=samples, elapsed_seconds=time.monotonic() - start)
    (pack / "pack.json").write_text(json.dumps(metadata, indent=2) + "\n")
    compare_sheet(manifest, graphics, pack, selected)
    print(json.dumps(metadata, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--graphics", type=Path, default=ROOT / "analysis/graphics")
    parser.add_argument("--pack", type=Path, default=ROOT / "analysis/graphics/hdpack")
    parser.add_argument("--samples", action="store_true")
    parser.add_argument("--tile", type=int, default=128)
    args = parser.parse_args()
    upscale(args.graphics, args.pack, args.samples, args.tile)
