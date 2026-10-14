#!/usr/bin/env python3
"""Refine only HUD/icon RGB via 4x inference downsampled to the existing 2x size."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

from PIL import Image, ImageDraw
from hdpack import validate
from upscale_graphics import ROOT, bleed_transparent_rgb


def refine(graphics, base, output):
    validate(base)
    if output.exists():
        raise ValueError(f"Preserving existing pack: {output}")
    manifest = json.loads((graphics / "manifest.json").read_text())
    pattern = re.compile(r"^res/texture_(?:quad_hud_group(?:0[1-7]|_wa)|quad_icons_inventory|markers)")
    selected = {Path(e["original"]).name: e for e in manifest["entries"] if pattern.search(e["source"])}
    if not selected:
        raise ValueError("No HUD/icon atlases found")
    work = graphics / "ui-refinement"
    inputs, results = work / "input", work / "output"
    inputs.mkdir(parents=True, exist_ok=True)
    results.mkdir(exist_ok=True)
    tool = ROOT / "analysis/deps/realesrgan-ncnn-vulkan-20220424/realesrgan-ncnn-vulkan"
    for filename, entry in selected.items():
        original = Image.open(graphics / entry["original"]).convert("RGBA")
        bleed_transparent_rgb(original).save(inputs / filename)
    print(f"Refining {len(selected)} UI atlases via 4x -> 2x; original alpha retained", flush=True)
    with (work / "inference.log").open("w") as log:
        subprocess.run([str(tool), "-i", str(inputs), "-o", str(results), "-m", str(tool.parent / "models"),
                        "-n", "realesr-animevideov3", "-s", "4", "-t", "128", "-j", "1:1:1", "-f", "png"],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    shutil.copytree(base, output)
    for filename, entry in selected.items():
        original = Image.open(graphics / entry["original"]).convert("RGBA")
        image = Image.open(results / filename).convert("RGB")
        if image.size != (original.width * 4, original.height * 4):
            raise ValueError(f"Unexpected AI dimensions: {filename}")
        size = (original.width * 2, original.height * 2)
        image = image.resize(size, Image.Resampling.LANCZOS)
        image.putalpha(original.getchannel("A").resize(size, Image.Resampling.LANCZOS))
        image.save(output / "images" / filename)
    untouched = 0
    for image in (base / "images").glob("*.png"):
        if image.name not in selected:
            if image.read_bytes() != (output / "images" / image.name).read_bytes():
                raise ValueError(f"Non-UI texture changed: {image.name}")
            untouched += 1
    metadata = json.loads((output / "pack.json").read_text())
    metadata["ui_refinement"] = dict(model="realesr-animevideov3-x4", output_scale=2,
        downsample="Lanczos", alpha="original-Lanczos-2x", atlases=[e["source"] for e in selected.values()],
        non_ui_images_unchanged=untouched,
        model_sha256=hashlib.sha256((tool.parent / "models/realesr-animevideov3-x4.bin").read_bytes()).hexdigest())
    (output / "pack.json").write_text(json.dumps(metadata, indent=2) + "\n")
    examples = list(selected.items())[:4]
    sheet = Image.new("RGB", (880, len(examples) * 300), "#253146")
    draw = ImageDraw.Draw(sheet)
    for row, (filename, entry) in enumerate(examples):
        draw.text((10, row * 300 + 5), Path(entry["source"].split("#")[0]).name, fill="white")
        for column, pack in enumerate((base, output)):
            image = Image.open(pack / "images" / filename).convert("RGBA")
            side = min(256, image.width, image.height)
            crop = image.crop((0, 0, side, side)).resize((256, 256), Image.Resampling.LANCZOS)
            sheet.paste(crop, (10 + column * 440, row * 300 + 35), crop)
            draw.text((10 + column * 440, row * 300 + 20), "AI 2x" if column == 0 else "AI 4x -> 2x", fill="white")
    sheet.save(work / "comparison.png")
    validate(output)
    print(f"Pack: {output}; {len(selected)} UI atlases refined; {untouched} other images byte-identical")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--graphics", type=Path, default=ROOT / "analysis/graphics")
    parser.add_argument("--base-pack", type=Path, default=ROOT / "analysis/graphics/hdpack")
    parser.add_argument("--output", type=Path, default=ROOT / "analysis/graphics/hdpack-ui")
    args = parser.parse_args()
    refine(args.graphics, args.base_pack, args.output)
