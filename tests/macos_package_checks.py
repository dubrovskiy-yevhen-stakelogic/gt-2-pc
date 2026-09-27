"""Check source identity and save-preserving app publication without Apple tools."""
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('mac_package', Path(__file__).resolve().parents[1] / 'scripts/package-macos.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class PackageChecks(unittest.TestCase):
    def test_source_line_endings_and_content(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            payload = b'line one\r\nline two\r\n'
            data = {'files': [{'path': 'source.cpp', 'sha256': hashlib.sha256(payload).hexdigest(),
                              'textSha256': hashlib.sha256(payload.replace(b'\r\n', b'\n')).hexdigest()}]}
            (root / 'SOURCE-MANIFEST.json').write_text(json.dumps(data))
            (root / 'source.cpp').write_bytes(payload)
            package.source_snapshot(root)
            (root / 'source.cpp').write_bytes(payload.replace(b'\r\n', b'\n'))
            package.source_snapshot(root)
            (root / 'source.cpp').write_text('modified source\n')
            with self.assertRaises(ValueError):
                package.source_snapshot(root)

    def test_manifest_path_escape(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'SOURCE-MANIFEST.json').write_text(json.dumps({'files': [
                {'path': '../outside', 'sha256': '0' * 64}]}))
            with self.assertRaises(ValueError):
                package.source_snapshot(root)

    def test_app_replacement_retains_backup(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); old = root / 'Applications/GT2.app'; new = root / 'new.app'
            for app, content in ((old, b'old'), (new, b'new')):
                (app / 'Contents').mkdir(parents=True)
                (app / 'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': 'io.github.gt2pc.macos'}))
                (app / 'Contents/game').write_bytes(content)
            saves = root / 'saves'; saves.mkdir(); (saves / 'card').write_bytes(b'keep')
            backup = package.install_app(new, old)
            self.assertEqual((old / 'Contents/game').read_bytes(), b'new')
            self.assertEqual((backup / 'Contents/game').read_bytes(), b'old')
            self.assertEqual((saves / 'card').read_bytes(), b'keep')


if __name__ == '__main__':
    unittest.main()
