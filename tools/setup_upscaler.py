#!/usr/bin/env python3
"""Install the official portable Real-ESRGAN ncnn Vulkan release locally."""
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
URL = "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-ubuntu.zip"
ARCHIVE_SHA256 = "e5aa6eb131234b87c0c51f82b89390f5e3e642b7b70f2b9bbe95b6a285a40c96"


def setup():
    parent = ROOT / "analysis/deps"
    parent.mkdir(parents=True, exist_ok=True)
    archive = parent / "realesrgan-ncnn-vulkan-20220424-ubuntu.zip"
    dest = parent / "realesrgan-ncnn-vulkan-20220424"
    if not archive.exists():
        with urllib.request.urlopen(URL, timeout=120) as source, archive.open("xb") as output:
            shutil.copyfileobj(source, output)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != ARCHIVE_SHA256:
        raise ValueError("Upscaler archive differs from the verified official release")
    if not dest.exists():
        with zipfile.ZipFile(archive) as z:
            for name in z.namelist():
                path = PurePosixPath(name)
                if path.is_absolute() or ".." in path.parts:
                    raise ValueError(f"Invalid tool archive path: {name}")
            z.extractall(dest)
    binaries = list(dest.rglob("realesrgan-ncnn-vulkan"))
    if len(binaries) != 1:
        raise ValueError("Could not identify the upscaler executable")
    binary = binaries[0]
    binary.chmod(0o755)
    model = binary.parent / "models/realesr-animevideov3-x2.bin"
    if not model.is_file() or not model.with_suffix(".param").is_file():
        raise ValueError("Official 2x model weights/parameters missing")
    report = dict(url=URL, archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
                  executable=str(binary.relative_to(ROOT)),
                  model_sha256=hashlib.sha256(model.read_bytes()).hexdigest(),
                  models=[str(p.relative_to(ROOT)) for p in dest.rglob("*.param")])
    (dest / "source.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    setup()
