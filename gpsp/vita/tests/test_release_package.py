"""Source and distribution contracts. RELEASE_VPK selects an assembled candidate."""

import hashlib
import os
from pathlib import Path, PurePosixPath
import re
import struct
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[3]
VITA = ROOT / "gpsp/vita"
VPK = Path(os.environ.get("RELEASE_VPK", ROOT / "VitAdhoc.vpk"))
RUNTIME_SHA256 = "29a82a3716664c1616e404702fe46ca0460d59e8242a5dfcbca63f3a87b2a6c8"  # version 1.1 (audio fixes)
NOTICES = {"licenses/GPL-2.0.txt", "licenses/MPL-2.0.txt", "licenses/Inter-OFL.txt",
           "licenses/Outfit-OFL.txt", "licenses/CC-BY-SA-4.0.txt", "notices/THIRD-PARTY.md",
           "notices/WIKIPEDIA-ATTRIBUTION.md", "licenses/GPL-3.0.txt",
           "licenses/GPU-xBRZ-notices.txt", "licenses/CPU-xBRZ-notice.txt",
           "licenses/vita2d-MIT.txt", "licenses/stb.txt",
           "licenses/VitaSDK-Headers-MIT.txt", "licenses/Newlib-Notices.txt",
           "licenses/GCC-Runtime-Exception.txt", "notices/VitaSDK-build-lock.json"}


class ReleasePackageTests(unittest.TestCase):
    def test_current_livearea_generator_is_the_assets_target(self):
        makefile = (VITA / "Makefile").read_text()
        recipe = re.search(r"^assets:\n((?:\t[^\n]*\n)+)", makefile, re.MULTILINE)
        self.assertIsNotNone(recipe)
        self.assertIn("tools/make_livearea.py", recipe.group(1))
        self.assertNotIn("tools/make_assets.py", recipe.group(1))

    def test_full_notices_and_font_source_are_available(self):
        for name in NOTICES:
            with self.subTest(name=name):
                self.assertTrue((ROOT / name).is_file())
        for name in ("Inter-OFL.txt", "Outfit-OFL.txt"):
            self.assertIn("SIL OPEN FONT LICENSE", (ROOT / "licenses" / name).read_text())
        for name in ("Inter-Regular.ttf", "Inter-SemiBold.ttf", "Outfit-Medium.ttf", "Outfit-SemiBold.ttf"):
            self.assertTrue((ROOT / "gpsp/assets/fonts" / name).is_file())

    def test_vendored_runtime_notices_are_preserved(self):
        for component, notice in (("vita2d", "vita2d-MIT.txt"), ("stb", "stb.txt")):
            with self.subTest(component=component):
                target = ROOT / "licenses" / notice
                self.assertTrue(target.is_file())
                self.assertEqual(target.read_bytes(), (VITA / "third_party" / component / "LICENSE").read_bytes())
        target = ROOT / "licenses/CPU-xBRZ-notice.txt"
        self.assertTrue(target.is_file())
        header = (VITA / "third_party/xbrz/xbrz.h").read_text().split("\n\n", 1)[0] + "\n"
        self.assertEqual(target.read_text(), header)

    def test_distribution_is_bios_and_rom_free_and_paths_are_unique(self):
        self.assertTrue(VPK.is_file(), "Assemble the candidate before package acceptance")
        with zipfile.ZipFile(VPK) as package:
            self.assertIsNone(package.testzip())
            names = package.namelist()
            self.assertEqual(len(names), len(set(name.casefold() for name in names)))
            for name in names:
                self.assertFalse(name.startswith("/"))
                self.assertNotIn("..", PurePosixPath(name).parts)
                self.assertNotIn("\\", name)
                self.assertNotIn(PurePosixPath(name).suffix.lower(), {".gba", ".gb", ".gbc", ".sav", ".srm"})
                self.assertNotEqual(PurePosixPath(name).name.lower(), "gba_bios.bin")
            self.assertTrue(NOTICES.issubset(names))
            for name in NOTICES:
                with self.subTest(notice=name):
                    self.assertEqual(package.read(name), (ROOT / name).read_bytes())
            self.assertEqual(hashlib.sha256(package.read("eboot.bin")).hexdigest(), RUNTIME_SHA256)
            self.assertEqual(package.read("gamedb.txt"), (VITA / "assets/gamedb.txt").read_bytes())
            self.assertEqual(package.read("cheats.db"), (VITA / "assets/cheats.db").read_bytes())

    def test_title_id_preserves_existing_installs(self):
        self.assertTrue(VPK.is_file())
        with zipfile.ZipFile(VPK) as package:
            data = package.read("sce_sys/param.sfo")
        magic, _, keys, values, count = struct.unpack_from("<5I", data)
        self.assertEqual(magic, 0x46535000)
        fields = {}
        for index in range(count):
            key, _, size, _, offset = struct.unpack_from("<HHIII", data, 20 + 16 * index)
            key = data[keys + key:data.index(b"\0", keys + key)].decode()
            fields[key] = data[values + offset:values + offset + size].rstrip(b"\0")
        self.assertEqual(fields["TITLE_ID"], b"GBAD00001")


if __name__ == "__main__":
    unittest.main()
