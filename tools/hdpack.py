#!/usr/bin/env python3
"""Validate/package the local AI pack or deploy it alongside the APK via ADB."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time
import zipfile

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
BASE = "/sdcard/Android/data/com.ts3wa.port/files"


def validate(pack):
    lines = (pack / "manifest.tsv").read_text().splitlines()
    if not lines or lines[0] != "TS3WA_HD1":
        raise ValueError("Unsupported HD pack")
    checked = set()
    hashes = set()
    for line in lines[1:]:
        key, width, height, premultiplied, filename = line.split()
        if not re.fullmatch(r"[0-9a-f]{16}", key) or key in hashes:
            raise ValueError("Invalid/duplicate original texture hash")
        if not re.fullmatch(r"images/[0-9a-f]{24}\.png", filename) or premultiplied not in ("0", "1"):
            raise ValueError("Invalid HD image path/alpha mode")
        hashes.add(key)
        if filename not in checked:
            with Image.open(pack / filename) as image:
                if image.size != (int(width) * 2, int(height) * 2) or image.mode != "RGBA":
                    raise ValueError(f"Unexpected dimensions/mode: {filename}")
                image.verify()
            checked.add(filename)
    metadata = json.loads((pack / "pack.json").read_text())
    if metadata["samples_only"]:
        raise ValueError("Sample pack is not the full release pack")
    if len(checked) != metadata["unique_images"] or len(hashes) != metadata["texture_upload_keys"]:
        raise ValueError("HD pack metadata mismatch")
    print(f"Validated HD pack: {len(checked)} RGBA images; {len(hashes)} original upload keys")
    return checked


def package(pack, output):
    images = validate(pack)
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        raise ValueError(f"Preserving existing archive: {output}")
    with zipfile.ZipFile(output, "x", compression=zipfile.ZIP_STORED) as z:
        for filename in sorted(images | {"manifest.tsv", "pack.json"}):
            z.write(pack / filename, "hdpack/" + filename)
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    output.with_suffix(output.suffix + ".sha256").write_text(f"{digest}  {output.name}\n")
    print(f"Local pack: {output}; {output.stat().st_size / (1 << 20):.1f} MiB; SHA-256 {digest}")


def deploy(pack, apk, serial):
    validate(pack)
    adb = ["adb"] + (["-s", serial] if serial else [])
    def call(*args, **kwargs):
        return subprocess.run(adb + list(args), check=True, **kwargs)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    backup = ROOT / "analysis/graphics" / ("device-before-hd-" + stamp)
    backup.mkdir(parents=True)
    call("shell", "test", "-d", BASE)
    call("shell", "am", "force-stop", "com.ts3wa.port")
    call("pull", BASE + "/home", str(backup / "home"))
    previous = backup / "config-original.txt"
    config = subprocess.run(adb + ["pull", BASE + "/config.txt", str(previous)], capture_output=True)
    lines = previous.read_text().splitlines() if config.returncode == 0 else []
    lines = [line for line in lines if not re.match(r"\s*(render_scale|texture_upscale)\s*=", line)]
    lines += ["render_scale=2", "texture_upscale=2"]
    new_config = backup / "config-hd.txt"
    new_config.write_text("\n".join(lines) + "\n")
    stage = BASE + "/hdpack-stage-" + stamp
    call("push", str(pack), stage)
    call("shell", "test", "-f", stage + "/manifest.tsv")
    call("shell", f"if [ -d {BASE}/hdpack ]; then mv {BASE}/hdpack {BASE}/hdpack-before-{stamp}; fi")
    call("shell", "mv", stage, BASE + "/hdpack")
    call("push", str(new_config), BASE + "/config.txt")
    call("install", "-r", str(apk))
    print(f"HD build installed; saves/config backed up at {backup}")
    print("Original bundle unchanged. Launch via com.ts3wa.port/.LauncherActivity.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("validate", "package", "deploy"))
    parser.add_argument("--pack", type=Path, default=ROOT / "analysis/graphics/hdpack")
    parser.add_argument("--output", type=Path, default=ROOT / "dist/TS3WorldAdventures-HD2x-local.zip")
    parser.add_argument("--apk", type=Path, default=ROOT / "android/app/build/outputs/apk/debug/app-debug.apk")
    parser.add_argument("--serial")
    args = parser.parse_args()
    if args.action == "validate":
        validate(args.pack)
    elif args.action == "package":
        package(args.pack, args.output)
    else:
        deploy(args.pack, args.apk, args.serial)
