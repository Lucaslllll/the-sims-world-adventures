#!/usr/bin/env python3
"""Fetch pinned Unicorn sources and build the isolated native 64-bit CPU probe.

No ADB calls, app installs or game content are included in the build products.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import struct
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
VERSION = "2.1.4"
URL = f"https://codeload.github.com/unicorn-engine/unicorn/tar.gz/refs/tags/{VERSION}"
SOURCE_SHA256 = "ea8863f095a0136388694e5a6063afd9bb7650e30243dd6251af59c5ce5601f4"


def build(host, jobs):
    deps = ROOT / "analysis/deps"
    deps.mkdir(parents=True, exist_ok=True)
    archive = deps / f"unicorn-{VERSION}.tar.gz"
    source = deps / f"unicorn-{VERSION}"
    if not archive.exists():
        temporary = archive.with_suffix(".download")
        with urllib.request.urlopen(URL, timeout=120) as src, temporary.open("wb") as dest:
            shutil.copyfileobj(src, dest)
        temporary.rename(archive)
    archive_hash = hashlib.sha256(archive.read_bytes()).hexdigest()
    if archive_hash != SOURCE_SHA256:
        raise ValueError("Unicorn source archive differs from the recorded 2.1.4 source")
    if not source.exists():
        with tarfile.open(archive) as tar:
            for entry in tar.getmembers():
                path = PurePosixPath(entry.name)
                if path.is_absolute() or ".." in path.parts or path.parts[0] != source.name:
                    raise ValueError(f"Unexpected source archive path: {entry.name}")
                if entry.issym() or entry.islnk():
                    raise ValueError(f"Unexpected source archive link: {entry.name}")
            tar.extractall(deps)
    sdk = Path.home() / "Android/Sdk"
    cmake = sdk / "cmake/3.22.1/bin/cmake"
    ninja = sdk / "cmake/3.22.1/bin/ninja"
    ndk = sdk / "ndk/27.2.12479018"
    target = "host64" if host else "android-arm64"
    output = ROOT / "analysis/executor-build" / target
    output.mkdir(parents=True, exist_ok=True)
    command = [str(cmake), "-S", str(ROOT / "experiments/arm64-executor"), "-B", str(output),
               "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja}", "-DCMAKE_BUILD_TYPE=Release",
               f"-DUNICORN_SOURCE_DIR={source}"]
    if not host:
        command += [f"-DCMAKE_TOOLCHAIN_FILE={ndk / 'build/cmake/android.toolchain.cmake'}",
                    "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-26",
                    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON"]
    subprocess.run(command, check=True)
    subprocess.run([str(cmake), "--build", str(output), "--target", "ts3wa_cpu_probe", "--parallel", str(jobs)], check=True)
    probe = output / "ts3wa_cpu_probe"
    library = output / "unicorn/libunicorn.so"
    if not host:
        for path in (probe, library):
            raw = path.read_bytes()
            if raw[:4] != b"\x7fELF" or raw[4] != 2 or struct.unpack_from("<H", raw, 18)[0] != 183:
                raise ValueError(f"Not an AArch64 ELF64: {path}")
    if host:
        subprocess.run([str(probe), str(ROOT / "analysis/sims3wa_armv6")], check=True)
    report = dict(target=target, unicorn_version=VERSION, source_url=URL,
                  source_archive_sha256=archive_hash, probe=str(probe.relative_to(ROOT)),
                  probe_sha256=hashlib.sha256(probe.read_bytes()).hexdigest(),
                  library=str(library.relative_to(ROOT)),
                  library_sha256=hashlib.sha256(library.read_bytes()).hexdigest(),
                  executed=host, original_game_content_in_build=False)
    (output / "build-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", action="store_true", help="Build and run x86-64/Linux probe instead of Android ARM64")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    build(args.host, args.jobs)
