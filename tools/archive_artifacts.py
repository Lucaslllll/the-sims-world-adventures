#!/usr/bin/env python3
"""Archive the standalone APK and selected game-only validation screenshots."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import zipfile
import re

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = {
    "boot-20261012-161611.png": "01-menu.png",
    "create-sim-v3.png": "02-criar-sim.png",
    "android-name.png": "03-teclado-android.png",
    "name-confirmed.png": "04-nome.png",
    "world-control.png": "05-mundo.png",
    "sofa-ready.png": "06-interacao.png",
    "reloaded-world.png": "07-save-carregado.png",
    "town-overview.png": "08-cidade.png",
}


def archive(apk, evidence, version="0.0.1-arm32-experimental"):
    with zipfile.ZipFile(apk) as z:
        names = z.namelist()
        if any(n.startswith("assets/") and not n.endswith("/") for n in names):
            raise ValueError("Revisar assets inesperados antes de publicar este APK.")
        binaries = {}
        for n in names:
            if n.startswith("lib/") and n.endswith(".so"):
                data = z.read(n)
                if data[:4] != b"\x7fELF" or data[4] != 1 or struct.unpack_from("<H", data, 18)[0] != 40:
                    raise ValueError(f"Biblioteca fora do alvo ARM32 experimental: {n}")
                binaries[n] = hashlib.sha256(data).hexdigest()
        if "lib/armeabi-v7a/libts3a.so" not in binaries:
            raise ValueError("Biblioteca do port ausente.")
    dist = ROOT / "dist"
    dist.mkdir(exist_ok=True)
    if not re.fullmatch(r"[A-Za-z0-9._-]+", version):
        raise ValueError("Invalid artifact version")
    dest = dist / f"TS3WorldAdventures-{version}.apk"
    digest = hashlib.sha256(apk.read_bytes()).hexdigest()
    if dest.exists() and hashlib.sha256(dest.read_bytes()).hexdigest() != digest:
        raise ValueError(f"Artefato diferente já existe: {dest}. Preserve-o e escolha nova versão.")
    if not dest.exists():
        shutil.copy2(apk, dest)
    (dist / (dest.name + ".sha256")).write_text(f"{digest}  {dest.name}\n")
    build_record = dist / ("build.json" if version == "0.0.1-arm32-experimental" else f"build-{version}.json")
    build_record.write_text(json.dumps(dict(apk=dest.name, sha256=digest,
        bytes=dest.stat().st_size, abi="armeabi-v7a", experimental=True,
        native_libraries=binaries, assets_files=0), indent=2) + "\n")
    if evidence:
        images = ROOT / "docs/img"
        images.mkdir(exist_ok=True)
        for source, target in EVIDENCE.items():
            dest_image = images / target
            original = ROOT / "analysis/device" / source
            if dest_image.exists():
                if dest_image.read_bytes() != original.read_bytes():
                    raise ValueError(f"Preservando evidência existente: {dest_image}")
            else:
                shutil.copy2(original, dest_image)
    print(f"APK: {dest}\nSHA-256: {digest}\nBytes: {dest.stat().st_size}; game assets in APK: 0")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apk", type=Path)
    parser.add_argument("--evidence", action="store_true")
    parser.add_argument("--version", default="0.0.1-arm32-experimental")
    args = parser.parse_args()
    archive(args.apk, args.evidence, args.version)
