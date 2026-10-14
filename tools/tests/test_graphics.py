"""Validate extraction, GPU upload identity and alpha-preserving AI outputs."""
import json
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
try:
    from PIL import Image
    import numpy
    from extract_graphics import m3g_images, texture_hash, M3G_MAGIC
    from hdpack import validate
except ModuleNotFoundError:
    Image = None

ROOT = Path(__file__).resolve().parents[2]
GRAPHICS = ROOT / "analysis/graphics"


@unittest.skipUnless(Image is not None, "Graphics requirements required")
class GraphicsExtraction(unittest.TestCase):
    def test_hash_preserves_format_and_dimensions(self):
        pixels = bytes([128, 17, 255, 128] * 4)
        self.assertNotEqual(texture_hash(2, 2, 0x1908, 0x1401, pixels), texture_hash(1, 4, 0x1908, 0x1401, pixels))
        self.assertNotEqual(texture_hash(2, 2, 0x1908, 0x1401, pixels), texture_hash(2, 2, 0x1907, 0x1401, pixels))

    def test_reject_truncated_m3g_payload(self):
        data = M3G_MAGIC + struct.pack("<BII", 0, 500, 487) + bytes(20)
        with self.assertRaisesRegex(ValueError, "section size"):
            list(m3g_images(data))

    @unittest.skipUnless((GRAPHICS / "manifest.json").exists(), "Local extraction required")
    def test_all_game_m3g_textures_decoded(self):
        manifest = json.loads((GRAPHICS / "manifest.json").read_text())
        self.assertEqual(sum(n for kind, n in manifest["containers"].items() if kind.startswith("M3G")), 270)
        self.assertEqual(manifest["containers"]["CgBI PNG"], 64)
        self.assertTrue(all(entry["source"].endswith(".dat") for entry in manifest["unsupported"]))

    @unittest.skipUnless((GRAPHICS / "hdpack/pack.json").exists(), "Local AI pack required")
    def test_pack_integrity_and_original_alpha(self):
        pack = GRAPHICS / "hdpack"
        validate(pack)
        manifest = json.loads((GRAPHICS / "manifest.json").read_text())
        # Transparent sprite atlas and clothing need original alpha, not AI-generated opacity.
        for name in ("texture_quad_hud_group02_cas_and_fe.m3g", "paintover_sm_torso_adventurer_black.png"):
            entry = next(e for e in manifest["entries"] if name in e["source"])
            original = Image.open(GRAPHICS / entry["original"]).convert("RGBA")
            result = Image.open(pack / "images" / Path(entry["original"]).name).convert("RGBA")
            expected = original.getchannel("A").resize(result.size, Image.Resampling.LANCZOS)
            self.assertEqual(result.getchannel("A").tobytes(), expected.tobytes())


if __name__ == "__main__":
    unittest.main()
