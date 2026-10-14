"""Integration proof: real game ARM/Thumb startup on a non-ARM host."""
import pathlib
import struct
import sys
import unittest
import zipfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
try:
    from emulate_boot import Guest
except ModuleNotFoundError:
    Guest = None

ROOT = pathlib.Path(__file__).resolve().parents[2]
IPA = ROOT / "The_Sims_3_World_Adventures__International__1.0.0_ios_2.2.1.ipa"


@unittest.skipUnless(Guest is not None and IPA.exists(), "Local IPA and research venv required")
class EmulatedStartup(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with zipfile.ZipFile(IPA) as z:
            cls.binary = z.read("Payload/sims3wa_iphone.app/sims3wa_iphone")

    def test_original_startup_reaches_uikit_boundary(self):
        result = Guest(self.binary).run(True, 5_000_000)
        self.assertEqual(result["original_constructors"], 28)
        self.assertEqual(result["ui_application_main"]["delegate"], "RuntimeAppDelegate")
        self.assertEqual(result["ui_application_main"]["argc"], 1)
        self.assertFalse(result["native_arm32_execution"])
        self.assertEqual(result["used_imports"]["_UIApplicationMain"], 1)
        # The game's original UIDevice(Hardware) category calls sysctl twice.
        self.assertEqual(result["used_imports"]["_sysctlbyname"], 2)

    def test_unknown_call_does_not_claim_success(self):
        guest = Guest(self.binary)
        del guest.handlers["_OSAtomicAdd32"]
        with self.assertRaisesRegex(RuntimeError, "Unimplemented guest import _OSAtomicAdd32"):
            guest.run(True, 5_000_000)
        self.assertIsNone(guest.ui_application)
        self.assertLess(len(guest.initializers), 28)

    def test_zero_double_clears_both_return_registers(self):
        from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1
        guest = Guest(self.binary)
        guest.call(guest.thunks["___floatsidfvfp"], [0, 0xffffffff, 0, 0], 100)
        self.assertEqual(guest.uc.reg_read(UC_ARM_REG_R0), 0)
        self.assertEqual(guest.uc.reg_read(UC_ARM_REG_R1), 0)


if __name__ == "__main__":
    unittest.main()
