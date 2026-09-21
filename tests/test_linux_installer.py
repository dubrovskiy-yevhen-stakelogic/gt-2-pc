"""Installer regression checks; no connected device or game data is required."""
import importlib.util
import gzip
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from gt2_disc import Disc, PROFILES, digest, resolve_image, safe_relative

spec = importlib.util.spec_from_file_location("installer", Path(__file__).resolve().parents[1] / "scripts/install-linux.py")
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_unsafe_paths(self):
        for name in ("../save", "/etc/passwd", "a/../b", "a//b", "C:/x", "a\\b", "", "a/./b", "a\0b"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                safe_relative(name)
        self.assertEqual(safe_relative(".text/data-race.txd"), ".text/data-race.txd")

    def test_cue_spaces_and_multitrack_rejection(self):
        binary = self.root / "My Game.bin"
        binary.write_bytes(b"test")
        cue = self.root / "My Game.cue"
        cue.write_text('FILE "My Game.bin" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\n')
        self.assertEqual(resolve_image(cue), binary.resolve())
        cue.write_text(cue.read_text() + " TRACK 02 AUDIO\n")
        with self.assertRaises(ValueError):
            resolve_image(cue)

    def test_reject_incomplete_disc_before_writing(self):
        binary = self.root / "truncated.bin"
        binary.write_bytes(bytes(2351))
        with self.assertRaises(ValueError):
            Disc(binary)
        binary.write_bytes(bytes(2352 * 17))
        with self.assertRaisesRegex(ValueError, "descriptor"):
            Disc(binary)

    def test_profile_table_matches_native_source(self):
        source = (Path(__file__).resolve().parents[1] / "src/gt2formats/exe_profile.cpp").read_text()
        for checksum in PROFILES:
            self.assertIn(checksum, source)

    def test_concatenated_gzip_uses_first_member_and_checks_crc(self):
        disc = object.__new__(Disc)
        disc.vol_lba = 0
        packed = gzip.compress(b"first asset") + gzip.compress(b"next asset")
        disc.form1 = lambda *_: packed
        self.assertEqual(disc.asset(("asset", 0, len(packed))), b"first asset")
        packed = bytearray(gzip.compress(b"first asset"))
        packed[-8] ^= 1
        import zlib
        with self.assertRaises(zlib.error):
            disc.asset(("asset", 0, len(packed)))

    def test_release_hash_failure(self):
        data = self.root / "data.txt"
        data.write_text("original")
        manifest = {"version": installer.VERSION, "package": installer.PACKAGE,
                    "files": [{"path": "data.txt", "sha256": digest(data)}]}
        (self.root / "release-manifest.json").write_text(json.dumps(manifest))
        data.write_text("corrupted")
        with self.assertRaisesRegex(ValueError, "verification"):
            installer.verify_release(self.root)

    def test_publish_preserves_saves_and_matching_hd(self):
        target = self.root / "arcade"
        target.mkdir()
        (target / "saves").mkdir()
        (target / "saves/career.sav").write_bytes(b"save")
        (target / "disc.raw2352").write_bytes(b"old")
        (target / "hd").mkdir()
        (target / "hd/profile.txt").write_text("same-profile")
        staged = self.root / "stage"
        staged.mkdir()
        (staged / "assets").mkdir()
        (staged / "assets/.carcolor").write_bytes(b"asset")
        (staged / "disc.raw2352").write_bytes(b"new")
        installer.publish_disc(staged, self.root, "arcade", "same-profile")
        self.assertEqual((target / "saves/career.sav").read_bytes(), b"save")
        self.assertEqual((target / "hd/profile.txt").read_text(), "same-profile")
        self.assertEqual((target / "disc.raw2352").read_bytes(), b"new")
        self.assertEqual(next(self.root.glob("backup-*/disc.raw2352")).read_bytes(), b"old")

    def test_publish_rolls_back_on_move_failure(self):
        target, staged = self.root / "arcade", self.root / "stage"
        for folder in (target, staged):
            (folder / "assets").mkdir(parents=True)
            (folder / "assets/test").write_text(folder.name)
            (folder / "disc.raw2352").write_text(folder.name)
        original = Path.rename

        def fail_new_disc(path, destination):
            if path == staged / "disc.raw2352":
                raise OSError("simulated disk failure")
            return original(path, destination)

        with patch.object(Path, "rename", fail_new_disc), self.assertRaises(OSError):
            installer.publish_disc(staged, self.root, "arcade", "profile")
        self.assertEqual((target / "disc.raw2352").read_text(), "arcade")
        self.assertEqual((target / "assets/test").read_text(), "arcade")

    def test_unauthorized_device_is_not_installed(self):
        result = subprocess.CompletedProcess([], 0, "List of devices attached\nquest\tunauthorized\n")
        with patch.object(subprocess, "run", return_value=result) as run, self.assertRaises(ValueError):
            installer.Adb("adb")
        self.assertEqual(run.call_count, 1)
        self.assertEqual(run.call_args.args[0], ["adb", "devices"])

    def test_signature_failure_stops_before_data_push(self):
        adb = object.__new__(installer.Adb)
        adb.executable, adb.serial = "adb", "quest"
        result = subprocess.CompletedProcess([], 1, "Failure [INSTALL_FAILED_UPDATE_INCOMPATIBLE]")
        with patch.object(subprocess, "run", return_value=result) as run, self.assertRaises(RuntimeError):
            installer.install_quest(adb, self.root, self.root, ["arcade"])
        self.assertEqual(run.call_count, 1)
        self.assertIn("install", run.call_args.args[0])
        self.assertNotIn("uninstall", run.call_args.args[0])

    def test_headset_flow_checks_hash_and_app_access(self):
        mode = self.root / "arcade"
        (mode / "assets").mkdir(parents=True)
        (mode / "assets/.carcolor").write_bytes(b"asset")
        (mode / "disc.raw2352").write_bytes(b"disc")
        commands = []

        def reply(command, **_):
            commands.append(command)
            if command[-1] == "devices":
                text = "List of devices attached\nquest\tdevice\n"
            elif "shell" in command and "sha256sum" in command[-1]:
                text = digest(mode / "disc.raw2352") + "  disc.raw2352\n"
            elif "shell" in command:
                text = "Broadcast completed: result=0"
            else:
                text = "Success"
            return subprocess.CompletedProcess(command, 0, text)

        with patch.object(subprocess, "run", side_effect=reply):
            adb = installer.Adb("adb")
            installer.install_quest(adb, self.root, self.root, ["arcade"])
        self.assertTrue(any("VERIFY_DATA" in str(c) for c in commands))
        self.assertFalse(any("uninstall" in c or "clear" in c for c in commands))
        self.assertFalse(any("am start" in str(c) for c in commands))


if __name__ == "__main__":
    unittest.main()
