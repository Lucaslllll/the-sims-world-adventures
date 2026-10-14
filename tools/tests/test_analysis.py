"""Regression checks against the actual local executable and malformed tables."""
import pathlib
import struct
import sys
import unittest
import zipfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from analyze_ipa import MachO32

ROOT = pathlib.Path(__file__).resolve().parents[2]
IPA = ROOT / "The_Sims_3_World_Adventures__International__1.0.0_ios_2.2.1.ipa"


@unittest.skipUnless(IPA.exists(), "Local user-supplied IPA required")
class BinaryAnalysis(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with zipfile.ZipFile(IPA) as z:
            cls.data = z.read("Payload/sims3wa_iphone.app/sims3wa_iphone")

    def test_real_binary_metadata(self):
        image = MachO32(self.data)
        self.assertEqual((image.subtype, image.cryptid, image.entry), (6, 0, 0x4b1c))
        self.assertEqual(len(image.imports), 480)
        self.assertEqual(len(image.relocations), 1245)
        self.assertTrue(all(s["type"] == 1 for s in image.imports))
        self.assertEqual(len(image.objc()["classes"]), 43)
        self.assertIn("_objc_msgSend_stret", [s["name"] for s in image.imports])
        main = [s for s in image.symbols if s["name"] == "_main" and s["type"] == 14]
        self.assertEqual(main[0]["value"], 0xcb1f0)

    def test_external_relocations_use_absolute_vm_addresses(self):
        image = MachO32(self.data)
        # UIKit class import: stored addend is zero at the relocation address.
        reloc = image.relocations[0]
        self.assertEqual(reloc["symbol"], "_OBJC_CLASS_$_CAEAGLLayer")
        self.assertEqual(image.word(reloc["address"]), 0)
        self.assertTrue(all(r["external"] and r["length"] == 2 and r["type"] == 0
                            for r in image.relocations))

    def test_truncated_header_and_tables(self):
        for length in (0, 4, 27, 64, len(self.data) - 1):
            with self.subTest(length=length), self.assertRaises(ValueError):
                MachO32(self.data[:length])

    def test_reject_wrong_architecture(self):
        data = bytearray(self.data)
        struct.pack_into("<I", data, 4, 0x100000c)
        with self.assertRaises(ValueError):
            MachO32(data)

    def test_invalid_command_size(self):
        data = bytearray(self.data)
        struct.pack_into("<I", data, 32, 0xffffffff)
        with self.assertRaises(ValueError):
            MachO32(data)

    def test_bad_symbol_string_offset(self):
        image = MachO32(self.data)
        at = 28
        data = bytearray(self.data)
        while struct.unpack_from("<I", data, at)[0] != 2:
            at += struct.unpack_from("<I", data, at + 4)[0]
        table = struct.unpack_from("<I", data, at + 8)[0]
        struct.pack_into("<I", data, table, 0xffffffff)
        with self.assertRaises(ValueError):
            MachO32(data)


if __name__ == "__main__":
    unittest.main()
