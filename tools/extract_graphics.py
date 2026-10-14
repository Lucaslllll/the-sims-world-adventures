#!/usr/bin/env python3
"""Extract original PNG/CgBI/TIFF, M3G Image2D (including PVRTC) and raw DAT graphics.

Leaves the user's IPA/bundle untouched. Records GL upload hashes and logical
dimensions so larger replacement textures do not change sprite coordinates.
"""
import argparse
from collections import Counter
import ctypes
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import zlib

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
M3G_MAGIC = b"\xabJSR184\xbb\r\n\x1a\n"
GL_RGB, GL_RGBA, GL_BYTE = 0x1907, 0x1908, 0x1401
_native_hash = None


def texture_hash(width, height, fmt, kind, raw):
    if _native_hash is not None:
        return f"{_native_hash(width, height, fmt, kind, raw, len(raw)):016x}"
    value = 14695981039346656037
    for byte in struct.pack("<4I", width, height, fmt, kind) + raw:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return f"{value:016x}"


class Decoders:
    def __init__(self, output):
        library = output / "libgraphics_decode.so"
        sources = [ROOT / "tools/graphics_decode.cpp", ROOT / "android/app/src/main/cpp/hle/pvrtc.cpp",
                   ROOT / "android/third_party/stb_image.h"]
        digest = hashlib.sha256(b"".join(p.read_bytes() for p in sources)).hexdigest()
        stamp = library.with_suffix(".sha256")
        if not library.exists() or not stamp.exists() or stamp.read_text() != digest:
            subprocess.run(["g++", "-std=c++20", "-O2", "-fPIC", "-shared",
                            "-I" + str(ROOT / "android/third_party"),
                            "-I" + str(ROOT / "android/app/src/main/cpp"),
                            str(sources[0]), str(sources[1]), "-o", str(library)], check=True)
            stamp.write_text(digest)
        self.lib = ctypes.CDLL(str(library))
        self.lib.ts3wa_decode_image.restype = ctypes.c_void_p
        self.lib.ts3wa_decode_image.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                                              ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int)]
        self.lib.ts3wa_image_free.argtypes = [ctypes.c_void_p]
        self.lib.ts3wa_decode_pvrtc.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                                               ctypes.c_int, ctypes.c_void_p]
        self.lib.ts3wa_texture_hash.argtypes = [ctypes.c_uint32] * 4 + [ctypes.c_void_p, ctypes.c_size_t]
        self.lib.ts3wa_texture_hash.restype = ctypes.c_uint64
        global _native_hash
        _native_hash = self.lib.ts3wa_texture_hash

    def png(self, data, unpremultiply=False):
        width, height = ctypes.c_int(), ctypes.c_int()
        pointer = self.lib.ts3wa_decode_image(data, len(data), unpremultiply, ctypes.byref(width), ctypes.byref(height))
        if not pointer:
            raise ValueError("stb could not decode the original image")
        try:
            raw = ctypes.string_at(pointer, width.value * height.value * 4)
            return Image.frombytes("RGBA", (width.value, height.value), raw)
        finally:
            self.lib.ts3wa_image_free(pointer)

    def pvrtc(self, data, width, height, rgb):
        expected = max(width // 4, 2) * max(height // 4, 2) * 8
        if len(data) < expected:
            raise ValueError("Incomplete PVRTC level zero")
        output = ctypes.create_string_buffer(width * height * 4)
        if not self.lib.ts3wa_decode_pvrtc(data, width, height, 0, output):
            raise ValueError("PVRTC decoding failed")
        image = Image.frombytes("RGBA", (width, height), output.raw)
        if rgb:
            image.putalpha(255)
        return image


def m3g_images(data):
    if not data.startswith(M3G_MAGIC):
        raise ValueError("Not a standard M3G container")
    section_at, object_index = 12, 0
    while section_at < len(data):
        if section_at + 13 > len(data):
            raise ValueError("Truncated M3G section")
        compression, total, unpacked = struct.unpack_from("<BII", data, section_at)
        if total < 13 or section_at + total > len(data):
            raise ValueError("Invalid M3G section size")
        payload = data[section_at + 9:section_at + total - 4]
        if compression == 1:
            payload = zlib.decompress(payload)
        elif compression != 0:
            raise ValueError("Unknown M3G compression")
        if len(payload) != unpacked:
            raise ValueError("M3G uncompressed size mismatch")
        at = 0
        while at < len(payload):
            if at + 5 > len(payload):
                raise ValueError("Truncated M3G object")
            kind, count = struct.unpack_from("<BI", payload, at)
            if at + 5 + count > len(payload):
                raise ValueError("Invalid M3G object length")
            obj = payload[at + 5:at + 5 + count]
            at += 5 + count
            object_index += 1
            if kind != 10:
                continue
            p = 4  # Object3D user ID
            tracks = struct.unpack_from("<I", obj, p)[0]
            p += 4 + tracks * 4
            params = struct.unpack_from("<I", obj, p)[0]
            p += 4
            for _ in range(params):
                length = struct.unpack_from("<I", obj, p + 4)[0]
                p += 8 + length
            fmt, mutable, width, height = struct.unpack_from("<BBII", obj, p)
            p += 10
            if not (0 < width <= 8192 and 0 < height <= 8192) or mutable:
                raise ValueError("Unsupported M3G Image2D dimensions/mutability")
            palette_size = struct.unpack_from("<I", obj, p)[0]
            p += 4
            palette = obj[p:p + palette_size]
            p += palette_size
            pixels_size = struct.unpack_from("<I", obj, p)[0]
            p += 4
            if p + pixels_size != len(obj):
                raise ValueError("M3G image payload length mismatch")
            yield object_index, fmt, width, height, palette, obj[p:]
        section_at += total


def premultiplied(image):
    pixels = np.array(image, dtype=np.uint16)
    pixels[:, :, :3] = (pixels[:, :, :3] * pixels[:, :, 3:4] + 127) // 255
    return pixels.astype(np.uint8).tobytes()


def upload_variants(image, raw_premultiplied=None):
    width, height = image.size
    rgba = image.tobytes()
    variants = []
    def add(fmt, kind, data, pre):
        variants.append(dict(hash=texture_hash(width, height, fmt, kind, data),
                             format=fmt, type=kind, premultiplied=pre))
    add(GL_RGBA, GL_BYTE, rgba, False)
    add(GL_RGBA, GL_BYTE, raw_premultiplied if raw_premultiplied is not None else premultiplied(image), True)
    px = np.asarray(image, dtype=np.uint16)
    if bool(np.all(px[:, :, 3] == 255)):
        add(GL_RGB, GL_BYTE, image.convert("RGB").tobytes(), False)
        rgb565 = ((px[:, :, 0] >> 3) << 11) | ((px[:, :, 1] >> 2) << 5) | (px[:, :, 2] >> 3)
        add(GL_RGB, 0x8363, rgb565.astype("<u2").tobytes(), False)
    rgba4444 = ((px[:, :, 0] >> 4) << 12) | ((px[:, :, 1] >> 4) << 8) | ((px[:, :, 2] >> 4) << 4) | (px[:, :, 3] >> 4)
    add(GL_RGBA, 0x8033, rgba4444.astype("<u2").tobytes(), False)
    rgba5551 = ((px[:, :, 0] >> 3) << 11) | ((px[:, :, 1] >> 3) << 6) | ((px[:, :, 2] >> 3) << 1) | (px[:, :, 3] >> 7)
    add(GL_RGBA, 0x8034, rgba5551.astype("<u2").tobytes(), False)
    # Fully opaque images have identical straight/premultiplied uploads.
    unique = {}
    for entry in variants:
        unique.setdefault(entry["hash"], entry)
    return list(unique.values())


def raw_dat_image(data):
    if len(data) < 20:
        raise ValueError("DAT header incomplete")
    size, width, height = struct.unpack_from("<3I", data)
    if not (0 < width <= 2048 and 0 < height <= 2048) or size != width * height * 2 or len(data) != 20 + size:
        raise ValueError("Compressed/proprietary DAT; no raw RGB565 payload")
    words = np.frombuffer(data[20:], dtype="<u2").reshape(height, width)
    pixels = np.empty((height, width, 4), dtype=np.uint8)
    r, g, b = (words >> 11) & 31, (words >> 5) & 63, words & 31
    pixels[:, :, 0] = (r << 3) | (r >> 2)
    pixels[:, :, 1] = (g << 2) | (g >> 4)
    pixels[:, :, 2] = (b << 3) | (b >> 2)
    pixels[:, :, 3] = 255
    return Image.fromarray(pixels), data[20:]


def extract(bundle, output):
    output.mkdir(parents=True, exist_ok=True)
    originals = output / "originals"
    originals.mkdir(exist_ok=True)
    decoder = Decoders(output)
    entries, failures, unique = [], [], {}

    def record(source, image, kind, variants):
        width, height = image.size
        digest = hashlib.sha256(struct.pack("<2I", width, height) + image.tobytes()).hexdigest()
        filename = digest[:24] + ".png"
        if digest not in unique:
            image.save(originals / filename)
            unique[digest] = filename
        entries.append(dict(source=source, container=kind, width=width, height=height,
                            original="originals/" + filename, rgba_sha256=digest, uploads=variants))

    for file in sorted(bundle.rglob("*")):
        if not file.is_file() or file.suffix.lower() not in (".png", ".tif", ".m3g", ".dat"):
            continue
        source = file.relative_to(bundle).as_posix()
        data = file.read_bytes()
        try:
            if file.suffix.lower() == ".png":
                is_cgbi = b"CgBI" in data[:48]
                raw = decoder.png(data, False)
                image = decoder.png(data, True) if is_cgbi else raw
                record(source, image, "CgBI PNG" if is_cgbi else "PNG", upload_variants(image, raw.tobytes() if is_cgbi else None))
            elif file.suffix.lower() == ".tif":
                with Image.open(file) as loaded:
                    image = loaded.convert("RGBA")
                record(source, image, "TIFF", upload_variants(image))
            elif file.suffix.lower() == ".dat":
                image, raw = raw_dat_image(data)
                w, h = image.size
                record(source, image, "DAT RGB565", [dict(hash=texture_hash(w, h, GL_RGB, 0x8363, raw), format=GL_RGB, type=0x8363, premultiplied=False)])
            else:
                for index, fmt, width, height, palette, pixels in m3g_images(data):
                    if fmt in (124, 125):
                        image = decoder.pvrtc(pixels, width, height, fmt == 124)
                        gl_format = 0x8c00 if fmt == 124 else 0x8c02
                        base_size = max(width // 4, 2) * max(height // 4, 2) * 8
                        variants = [dict(hash=texture_hash(width, height, gl_format, 0, pixels[:base_size]), format=gl_format, type=0, premultiplied=False)]
                        variants += upload_variants(image)
                        kind = "M3G PVRTC4 RGB" if fmt == 124 else "M3G PVRTC4 RGBA"
                    elif fmt in (99, 100):
                        mode, channels = ("RGB", 3) if fmt == 99 else ("RGBA", 4)
                        if palette:
                            lookup = np.frombuffer(palette, dtype=np.uint8).reshape(-1, channels)
                            expanded = lookup[np.frombuffer(pixels, dtype=np.uint8)]
                            pixels = expanded.tobytes()
                        if len(pixels) != width * height * channels:
                            raise ValueError("M3G raw/palette image length mismatch")
                        image = Image.frombytes(mode, (width, height), pixels).convert("RGBA")
                        variants = upload_variants(image)
                        kind = "M3G " + mode
                    else:
                        raise ValueError(f"Unsupported M3G Image2D format {fmt}")
                    record(source + f"#image-{index}", image, kind, variants)
        except (ValueError, OSError, struct.error, zlib.error) as error:
            failures.append(dict(source=source, reason=str(error)))
    report = dict(bundle=str(bundle), images=len(entries), unique_images=len(unique),
                  containers=dict(Counter(e["container"] for e in entries)),
                  entries=entries, unsupported=failures)
    (output / "manifest.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    selected_names = ("texture_quad_hud_group02_cas_and_fe.m3g", "texture_objects.m3g", "texture_env_cas_room.m3g",
                      "texture_townmap_ssv.m3g", "opaque_sm_naked_head_skin01.png", "paintover_sm_torso_adventurer_black.png")
    sheet = Image.new("RGB", (900, 600), "#253146")
    draw = ImageDraw.Draw(sheet)
    for index, name in enumerate(selected_names):
        entry = next(e for e in entries if name in e["source"])
        image = Image.open(output / entry["original"]).convert("RGBA")
        image.thumbnail((280, 260), Image.Resampling.NEAREST)
        x, y = (index % 3) * 300 + 10, (index // 3) * 300 + 10
        sheet.paste(image, (x, y), image)
        draw.text((x, y + 265), name[:40], fill="white")
    sheet.save(output / "original-contact-sheet.png")
    print(json.dumps({k: report[k] for k in ("images", "unique_images", "containers")}, indent=2))
    print(f"Unsupported DAT/other containers: {len(failures)}; details in {output / 'manifest.json'}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "analysis/graphics")
    args = parser.parse_args()
    extract(args.bundle, args.output)
