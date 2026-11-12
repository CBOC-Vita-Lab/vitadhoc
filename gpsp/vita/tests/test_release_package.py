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
# RELEASE_VPK names the candidate to accept (release time: the committed root
# VitAdhoc.vpk, `RELEASE_VPK=VitAdhoc.vpk`): every check, the pinned hashes too.
# Without it (development) the package built here by `make vpk`
# (gpsp/vita/VitAdhoc.vpk) is checked, except for the hash pins, which only a
# release candidate matches; with no build the package checks are skipped.
if os.environ.get("RELEASE_VPK"):
    VPK = Path(os.environ["RELEASE_VPK"])
    if not VPK.is_absolute():
        VPK = ROOT / VPK
    PINNED = True
else:
    VPK = VITA / "VitAdhoc.vpk"
    PINNED = False
RUNTIME_SHA256 = "43f037b19c37ef0647ab637289c0e065ab2d60cd3aa60a9103af2a4896b3df8b"  # version 1.3.1
SNES_SHA256 = "19f4adb8c0d99c0793757cba92632dbf7f406bfc27f73eab0d61a15d339f92b4"  # version 1.3.1
NOTICES = {"licenses/GPL-2.0.txt", "licenses/MPL-2.0.txt", "licenses/Inter-OFL.txt",
           "licenses/Outfit-OFL.txt", "licenses/CC-BY-SA-4.0.txt", "notices/THIRD-PARTY.md",
           "notices/WIKIPEDIA-ATTRIBUTION.md", "licenses/GPL-3.0.txt",
           "licenses/GPU-xBRZ-notices.txt", "licenses/CPU-xBRZ-notice.txt",
           "licenses/vita2d-MIT.txt", "licenses/stb.txt",
           "licenses/VitaSDK-Headers-MIT.txt", "licenses/Newlib-Notices.txt",
           "licenses/GCC-Runtime-Exception.txt", "notices/VitaSDK-build-lock.json",
           "notices/SNES-Snes9x.md", "notices/NES-FCEUmm.md",
           "licenses/FCEUmm-GPL-2.0.txt", "licenses/libretro-common-MIT.txt",
           "notices/WSWAN-Beetle.md", "licenses/Beetle-WonderSwan-GPL-2.0.txt",
           "licenses/LGPL-2.1.txt"}
# The SNES player's licence travels in the package under these names.
SNES_NOTICES = {"licenses/Snes9x-LICENSE.txt": "snes/core/LICENSE",
                "licenses/Snes9x-PROVENANCE.md": "snes/core/PROVENANCE.md"}


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
        if not PINNED and not VPK.is_file():
            self.skipTest("no VPK built here (make -C gpsp/vita vpk); RELEASE_VPK for a release")
        self.assertTrue(VPK.is_file(), "Assemble the candidate before package acceptance")
        with zipfile.ZipFile(VPK) as package:
            self.assertIsNone(package.testzip())
            names = package.namelist()
            self.assertEqual(len(names), len(set(name.casefold() for name in names)))
            for name in names:
                self.assertFalse(name.startswith("/"))
                self.assertNotIn("..", PurePosixPath(name).parts)
                self.assertNotIn("\\", name)
                self.assertNotIn(PurePosixPath(name).suffix.lower(),
                                 {".gba", ".gb", ".gbc", ".sfc", ".smc", ".nes", ".fds", ".unf",
                                  ".unif", ".ws", ".wsc", ".pc2", ".sav", ".srm"})
                self.assertNotEqual(PurePosixPath(name).name.lower(), "gba_bios.bin")
            self.assertTrue(NOTICES.issubset(names))
            for name in NOTICES:
                with self.subTest(notice=name):
                    self.assertEqual(package.read(name), (ROOT / name).read_bytes())
            if PINNED:
                self.assertEqual(hashlib.sha256(package.read("eboot.bin")).hexdigest(), RUNTIME_SHA256)
                self.assertEqual(hashlib.sha256(package.read("snes.bin")).hexdigest(), SNES_SHA256)
            else:
                self.assertIn("eboot.bin", names)
                self.assertIn("snes.bin", names)
            for name, source in SNES_NOTICES.items():
                with self.subTest(notice=name):
                    self.assertEqual(package.read(name), (ROOT / source).read_bytes())
            self.assertEqual(package.read("gamedb.txt"), (VITA / "assets/gamedb.txt").read_bytes())
            self.assertEqual(package.read("cheats.db"), (VITA / "assets/cheats.db").read_bytes())

    def test_title_id_preserves_existing_installs(self):
        if not PINNED and not VPK.is_file():
            self.skipTest("no VPK built here")
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
