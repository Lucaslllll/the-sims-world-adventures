#!/usr/bin/env python3
"""Import only the source/build scaffold from the local Ambitions reference.

Does not import game content, analysis, builds, saves or signing credentials.
Run once before applying the World Adventures-specific patches.
"""

import argparse
import hashlib
import json
from pathlib import Path
import shutil


def bootstrap(reference, destination, resume=False):
    source = reference / "android"
    target = destination / "android"
    if target.exists() and not resume:
        raise ValueError(f"Preservando projeto existente: {target}")
    ignored = shutil.ignore_patterns("build", ".cxx", ".gradle", ".git", "keystore",
                                     "keystore.properties", "local.properties")
    shutil.copytree(source, target, ignore=ignored, dirs_exist_ok=resume)
    # Rename our wrapper, including its Java/JNI package, never the original game.
    replacements = (("com.ts3ambitions.port", "com.ts3wa.port"),
                    ("com/ts3ambitions/port", "com/ts3wa/port"),
                    ("com_ts3ambitions_port", "com_ts3wa_port"),
                    ("sims3dp_iphone", "sims3wa_iphone"),
                    ("TS3Ambitions", "TS3WorldAdventures"),
                    ("The Sims 3 Ambitions", "The Sims 3 World Adventures"),
                    ("The Sims 3 Ambições", "The Sims 3 World Adventures"),
                    ("TS3 Ambitions", "TS3 World Adventures"),
                    ("TS3 Ambições", "TS3 World Adventures"),
                    ("TS3A", "TS3WA"))
    origin = {}
    for path in target.rglob("*"):
        if not path.is_file() or "third_party" in path.parts:
            continue
        relative = path.relative_to(target).as_posix()
        origin[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
        if path.suffix in (".cpp", ".h", ".S", ".java", ".xml", ".gradle", ".properties", ".txt"):
            text = path.read_text()
            for old, new in replacements:
                text = text.replace(old, new)
            path.write_text(text)
    old_java = target / "app/src/main/java/com/ts3ambitions/port"
    new_java = target / "app/src/main/java/com/ts3wa/port"
    new_java.parent.mkdir(parents=True, exist_ok=True)
    old_java.rename(new_java)
    (target / "local.properties").write_text(f"sdk.dir={Path.home() / 'Android/Sdk'}\n")
    (destination / "docs/reference-source.json").write_text(json.dumps({
        "reference": reference.name, "source_sha256_before_renaming": origin,
        "notes": "Source reuse from local reference; no EA content or signing keys imported."
    }, indent=2) + "\n")
    print(f"Base importada: {target}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("--resume", action="store_true", help="Retomar somente uma importação interrompida, antes dos patches")
    args = parser.parse_args()
    bootstrap(args.reference.resolve(), Path(__file__).resolve().parents[1], args.resume)
