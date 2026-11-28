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
RUNTIME_SHA256 = "8cbee9bfc7396232d3322d074e75699c19e51cfe17b8059e3288306a3a1db071"  # version 2.0.1
SNES_SHA256 = "ed456a6dd305c21457732592b505ca9b6245dd4313ef9543fc794bde4351bb89"  # version 2.0.1
MD_SHA256 = "d4c84436c1eb9830368be9607620e6e1b07b160ed98d6a901a2ba3c9c1a44a89"  # version 2.0.1
NOTICES = {"licenses/GPL-2.0.txt", "licenses/MPL-2.0.txt", "licenses/Inter-OFL.txt",
           "licenses/Outfit-OFL.txt", "licenses/NotoSansCJK-OFL.txt",
           "licenses/CC-BY-SA-4.0.txt", "notices/THIRD-PARTY.md",
           "notices/WIKIPEDIA-ATTRIBUTION.md", "licenses/GPL-3.0.txt",
           "licenses/CPU-xBRZ-notice.txt",
           "licenses/vita2d-MIT.txt", "licenses/stb.txt",
           "licenses/MMPX-MIT.txt", "licenses/xBR-MIT.txt",
           "licenses/SameBoy-Expat.txt", "licenses/CBOC-tools-MIT.txt",
           "licenses/VitaSDK-Headers-MIT.txt", "licenses/Newlib-Notices.txt",
           "licenses/GCC-Runtime-Exception.txt", "notices/VitaSDK-build-lock.json",
           "notices/GB-Gambatte.md", "licenses/Gambatte-GPL-2.0.txt",
           "notices/SNES-Snes9x.md", "notices/NES-FCEUmm.md",
           "licenses/FCEUmm-GPL-2.0.txt", "licenses/libretro-common-MIT.txt",
           "notices/WSWAN-Beetle.md", "licenses/Beetle-WonderSwan-GPL-2.0.txt",
           "notices/MD-GenesisPlusGX.md", "licenses/GenesisPlusGX-LICENSE.txt",
           "licenses/SegaCD-disc-readers.txt", "licenses/MbedTLS-LICENSE.txt",
           "licenses/LGPL-2.1.txt"}
# The SNES and Sega players' licences and provenances travel in the package under these names.
SNES_NOTICES = {"licenses/Snes9x-LICENSE.txt": "snes/core/LICENSE",
                "licenses/Snes9x-PROVENANCE.md": "snes/core/PROVENANCE.md"}
MD_NOTICES = {"licenses/GenesisPlusGX-PROVENANCE.md": "md/core/PROVENANCE.md"}
PACKED_FONTS = ("cjkfont.otf", "cjkfont-sc.otf", "latinfont.ttf", "latinfont-sb.ttf")
FORBIDDEN_SUFFIXES = {".gba", ".gb", ".gbc", ".sfc", ".smc", ".nes", ".fds", ".unf",
                      ".unif", ".ws", ".wsc", ".pc2", ".md", ".gen", ".smd", ".sms",
                      ".gg", ".sg", ".sc", ".chd", ".iso", ".cue", ".68k", ".m3u",
                      ".sav", ".srm"}
FORBIDDEN_BIOS_NAMES = {"gba_bios.bin", "bios_cd_u.bin", "bios_cd_e.bin", "bios_cd_j.bin",
                        "us_scd1_9210.bin", "us_scd2_9303.bin",
                        "eu_mcd1_9210.bin", "eu_mcd2_9303.bin",
                        "jp_mcd1_9111.bin", "jp_mcd1_9112.bin", "jp_mcd2_921222.bin",
                        "segacd_bios_us.bin", "megacd_bios_eu.bin", "megacd_bios_jp.bin"}


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
        for source in (*SNES_NOTICES.values(), *MD_NOTICES.values(),
                       "md/core/LICENSE.txt",
                       "gpsp/gbcore/gambatte/COPYING",
                       "gpsp/gbcore/gambatte/PROVENANCE.md"):
            with self.subTest(source=source):
                self.assertTrue((ROOT / source).is_file())
        for name in ("Inter-OFL.txt", "Outfit-OFL.txt", "NotoSansCJK-OFL.txt"):
            self.assertIn("SIL OPEN FONT LICENSE", (ROOT / "licenses" / name).read_text())
        for name in ("Inter-Regular.ttf", "Inter-SemiBold.ttf", "Outfit-Medium.ttf", "Outfit-SemiBold.ttf"):
            self.assertTrue((ROOT / "gpsp/assets/fonts" / name).is_file())
        for name in PACKED_FONTS:
            with self.subTest(font=name):
                self.assertTrue((VITA / "assets" / name).is_file())

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
        self.assertTrue((ROOT / "licenses/GenesisPlusGX-LICENSE.txt").read_bytes().startswith(
            (ROOT / "md/core/LICENSE.txt").read_bytes()))
        self.assertTrue((ROOT / "licenses/Gambatte-GPL-2.0.txt").read_bytes().endswith(
            (ROOT / "gpsp/gbcore/gambatte/COPYING").read_bytes()))

    def test_distribution_is_bios_and_rom_free_and_paths_are_unique(self):
        if not PINNED and not VPK.is_file():
            self.skipTest("no VPK built here (make -C gpsp/vita vpk); RELEASE_VPK for a release")
        self.assertTrue(VPK.is_file(), "Assemble the candidate before package acceptance")
        allowed_md = {n for n in (NOTICES | set(SNES_NOTICES) | set(MD_NOTICES)) if n.lower().endswith(".md")}
        with zipfile.ZipFile(VPK) as package:
            self.assertIsNone(package.testzip())
            names = package.namelist()
            self.assertEqual(len(names), len(set(name.casefold() for name in names)))
            for name in names:
                self.assertFalse(name.startswith("/"))
                self.assertNotIn("..", PurePosixPath(name).parts)
                self.assertNotIn("\\", name)
                if name not in allowed_md:
                    self.assertNotIn(PurePosixPath(name).suffix.lower(), FORBIDDEN_SUFFIXES)
                self.assertNotIn(PurePosixPath(name).name.lower(), FORBIDDEN_BIOS_NAMES)
            self.assertTrue(NOTICES.issubset(names))
            for name in NOTICES:
                with self.subTest(notice=name):
                    self.assertEqual(package.read(name), (ROOT / name).read_bytes())
            if PINNED:
                self.assertEqual(hashlib.sha256(package.read("eboot.bin")).hexdigest(), RUNTIME_SHA256)
                self.assertEqual(hashlib.sha256(package.read("snes.bin")).hexdigest(), SNES_SHA256)
                self.assertIsNotNone(MD_SHA256, "Pin md.bin with tools/pin-release.py before release")
                self.assertEqual(hashlib.sha256(package.read("md.bin")).hexdigest(), MD_SHA256)
            else:
                self.assertIn("eboot.bin", names)
                self.assertIn("snes.bin", names)
                self.assertIn("md.bin", names)
            for name, source in (*SNES_NOTICES.items(), *MD_NOTICES.items()):
                with self.subTest(notice=name):
                    self.assertEqual(package.read(name), (ROOT / source).read_bytes())
            for name in PACKED_FONTS:
                with self.subTest(font=name):
                    self.assertEqual(package.read(name), (VITA / "assets" / name).read_bytes())
            self.assertEqual(package.read("gamedb.txt"), (VITA / "assets/gamedb.txt").read_bytes())
            self.assertEqual(package.read("cheats.db"), (VITA / "assets/cheats.db").read_bytes())
            # one interface-language catalogue per file in lang/, byte for byte
            catalogues = sorted((ROOT / "lang").glob("*.txt"))
            self.assertTrue((ROOT / "lang/en.txt") in catalogues)
            for path in catalogues:
                with self.subTest(catalogue=path.name):
                    self.assertEqual(package.read("lang/" + path.name), path.read_bytes())
            self.assertFalse([n for n in names if n.startswith("lang/") and n not in
                              {"lang/" + p.name for p in catalogues}])

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
        self.assertEqual(fields["APP_VER"], b"02.01")


if __name__ == "__main__":
    unittest.main()
