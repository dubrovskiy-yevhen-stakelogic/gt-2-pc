import importlib.util
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('package_vr', Path(__file__).resolve().parents[1] / 'scripts/package-vr.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

class PackageChecks(unittest.TestCase):
    def fixture(self, base, corrupt=False):
        payload = b'powershell -File scripts/install-player.ps1 -Target PCVR'
        manifest = dict(version='0.9.2', files=[dict(path='INSTALL-PCVR.bat', sha256=hashlib.sha256(payload).hexdigest())])
        windows = base / 'windows.zip'
        with zipfile.ZipFile(windows, 'w') as z:
            z.writestr('windows/INSTALL-PCVR.bat', b'corrupt' if corrupt else payload)
            z.writestr('windows/release-manifest.json', json.dumps(manifest))
        apk = base / 'GT2-VR-0.9.2.apk'
        apk.write_bytes(b'signed apk fixture')
        return windows, apk

    def test_combined_archive_and_manifest(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            windows, apk = self.fixture(base)
            output = m.package(windows, apk, base / 'out')
            self.assertEqual(output.name, 'GT2-VR-0.9.2.zip')
            with zipfile.ZipFile(output) as z:
                prefix = 'GT2-VR-0.9.2/'
                manifest = json.loads(z.read(prefix + 'release-manifest.json'))
                for e in manifest['files']:
                    self.assertEqual(hashlib.sha256(z.read(prefix + e['path'])).hexdigest(), e['sha256'])
                self.assertEqual(z.read(prefix + apk.name), apk.read_bytes())
                self.assertIn(b'-Target Quest', z.read(prefix + 'INSTALL.bat'))
                self.assertIn(b'-Target Quest', z.read(prefix + 'INSTALL-QUEST.bat'))
                self.assertIn(b'-Target PCVR', z.read(prefix + 'INSTALL-PCVR.bat'))

    def test_rejects_corrupt_windows_package(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            windows, apk = self.fixture(base, corrupt=True)
            with self.assertRaisesRegex(ValueError, 'checksum'):
                m.package(windows, apk, base / 'out')

    def test_rejects_different_versions(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            windows, apk = self.fixture(base)
            other = apk.rename(base / 'GT2-VR-0.9.1.apk')
            with self.assertRaisesRegex(ValueError, 'versions differ'):
                m.package(windows, other, base / 'out')

if __name__ == '__main__':
    unittest.main()
