"""Host checks for HD publication, media retention and archive validation."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

spec = importlib.util.spec_from_file_location('mac_hd', Path(__file__).resolve().parents[1] / 'scripts/prepare-hd-macos.py')
hd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hd)


class MediaChecks(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.disc = self.root / 'arcade'
        self.disc.mkdir()
        (self.disc / 'disc.raw2352').write_bytes(b'disc fixture')
        (self.root / 'saves').mkdir()
        (self.root / 'saves/card.mcd').write_bytes(b'save fixture')
        self.profile = '1' * 40
        self.old = self.disc / 'hd'
        self.old.mkdir()
        (self.old / 'profile.txt').write_text(self.profile)
        (self.old / 'movies').mkdir()
        (self.old / 'movies/24.gtm').write_bytes(b'old movie')
        (self.old / 'startup.gtm').write_bytes(b'old startup')
        self.calls = []

    def fake_run(self, *args, capture=False):
        self.calls.append(args)
        command = str(args[1])
        if command == 'inspect':
            return json.dumps({'exeSha1': self.profile})
        if command == 'images':
            out = Path(args[3]); out.mkdir(parents=True, exist_ok=True)
            (out / 'profile.txt').write_text(self.profile)
            (out / 'test.png').write_bytes(b'original fixture')
        elif command == '-i':
            source, out = Path(args[2]), Path(args[4])
            for image in source.glob('*.png'):
                (out / image.name).write_bytes(b'enlarged fixture')
        elif command in ('ui', 'fonts'):
            out = Path(args[3]); out.mkdir(parents=True, exist_ok=True)
            key = '0123456789abcdef' + ('-1234' if command == 'fonts' else '')
            header = 'palette-contours4x-v1' if command == 'fonts' else 'indexed-scale4x-v2'
            (out / 'index.txt').write_text(header + '\n' + key + '\n')
            (out / (key + '.png')).write_bytes(b'atlas fixture')
        elif command == 'movie':
            out = Path(args[4]); out.mkdir(parents=True)
            for name in ('movie.txt', 'audio.pcm', '000001.png'):
                (out / name).write_bytes(b'movie fixture')
        elif command == 'pack':
            self.assertEqual(len(args), 5)  # Original frames constrain neural video changes.
            Path(args[3]).write_bytes(b'new packed movie')

    def prepare(self, mode='Menus'):
        hd.prepare_disc(self.root, 'arcade', (Path('media'), Path('inspect')), Path('upscale'), mode, None)

    def test_pictures_preserve_movies_startup_saves_and_disc(self):
        with patch.object(hd, 'run', self.fake_run):
            self.prepare()
        self.assertEqual((self.old / 'movies/24.gtm').read_bytes(), b'old movie')
        self.assertEqual((self.old / 'startup.gtm').read_bytes(), b'old startup')
        self.assertEqual((self.root / 'saves/card.mcd').read_bytes(), b'save fixture')
        self.assertEqual((self.disc / 'disc.raw2352').read_bytes(), b'disc fixture')
        self.assertEqual(len(list(self.disc.glob('hd.backup-*'))), 1)
        data = json.loads((self.old / 'manifest.json').read_text())
        self.assertTrue(any(f['path'] == 'fonts/0123456789abcdef-1234.png' for f in data['files']))
        for entry in data['files']:
            self.assertEqual(hd.digest(self.old / entry['path']), entry['sha256'])

    def test_upscale_failure_keeps_old_pack(self):
        def fail(*args, **kwargs):
            if args[1] == '-i':
                raise OSError('upscaler failed')
            return self.fake_run(*args, **kwargs)
        with patch.object(hd, 'run', fail), self.assertRaises(OSError):
            self.prepare()
        self.assertEqual((self.old / 'movies/24.gtm').read_bytes(), b'old movie')
        self.assertFalse(list(self.disc.glob('hd.backup-*')))

    def test_movies_pack_with_original_frames(self):
        with patch.object(hd, 'run', self.fake_run):
            self.prepare('MenusAndMovies')
        self.assertEqual(len([a for a in self.calls if a[1] == 'pack']), 3)
        for number in (24, 25, 26):
            self.assertEqual((self.old / 'movies' / (str(number) + '.gtm')).read_bytes(), b'new packed movie')

    def test_resume_reuses_finished_work(self):
        with patch.object(hd, 'run', self.fake_run):
            self.prepare('MenusAndMovies')
            self.calls.clear()
            self.prepare('MenusAndMovies')
        self.assertFalse([a for a in self.calls if a[1] in ('-i', 'pack', 'ui', 'fonts')])

    def test_invalid_font_index_is_rejected(self):
        folder = self.root / 'bad'; folder.mkdir()
        (folder / 'index.txt').write_text('palette-contours4x-v1\n../outside\n')
        with self.assertRaises(ValueError):
            hd.validate_index(folder, 'palette-contours4x-v1')

    def test_publish_failure_restores_previous_pack(self):
        stage = self.disc / '.candidate'; stage.mkdir()
        original = Path.rename
        def rename(path, target):
            if path == stage:
                raise OSError('publication failed')
            return original(path, target)
        with patch.object(Path, 'rename', rename), self.assertRaises(OSError):
            hd.publish(stage, self.old)
        self.assertEqual((self.old / 'movies/24.gtm').read_bytes(), b'old movie')

    def test_archive_paths_and_models(self):
        archive = self.root / 'tool.zip'
        with zipfile.ZipFile(archive, 'w') as z:
            z.writestr('../outside', 'bad')
        with self.assertRaises(ValueError):
            hd.extract_tool(archive, self.root / 'unpacked')
        with zipfile.ZipFile(archive, 'w') as z:
            z.writestr('tool/realesrgan-ncnn-vulkan', 'executable fixture')
            for suffix in ('.param', '.bin'):
                z.writestr('tool/models/realesrgan-x4plus' + suffix, 'model fixture')
        self.assertEqual(hd.extract_tool(archive, self.root / 'valid').name, 'realesrgan-ncnn-vulkan')


if __name__ == '__main__':
    unittest.main()
