"""Verify installed HD packs against pictures exported from each installed disc.

Requires Pillow. Uses only local media; does not install or launch the game.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from PIL import Image


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--media', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = {}
    for mode in ('arcade', 'simulation'):
        disc = args.runtime.resolve() / mode
        hd = disc / 'hd'
        exported = output / (mode + '-original')
        subprocess.run([str(args.media.resolve()), 'images', str(disc / 'disc.raw2352'), str(exported)], check=True)
        expected = {p.name for p in exported.glob('*.png')}
        actual = {p.name for p in (hd / 'images').glob('*.png')}
        assert expected and actual == expected, (mode, expected - actual, actual - expected)
        manifest = json.loads((hd / 'manifest.json').read_text(encoding='utf-8-sig'))
        profile = (exported / 'profile.txt').read_text().strip()
        assert manifest['profile'] == profile == (hd / 'profile.txt').read_text().strip()
        listed = {e['path'] for e in manifest['files']}
        disk_files = {p.relative_to(hd).as_posix() for p in hd.rglob('*') if p.is_file() and p != hd / 'manifest.json'}
        assert listed == disk_files and len(listed) == len(manifest['files']), mode
        for entry in manifest['files']:
            relative = Path(entry['path'])
            assert not relative.is_absolute() and '..' not in relative.parts
            assert digest(hd / relative) == entry['sha256'], entry['path']
        for name in sorted(expected):
            with Image.open(exported / name) as original, Image.open(hd / 'images' / name) as enhanced:
                enhanced.load()
                assert 0 < enhanced.width <= 2048 and 0 < enhanced.height <= 2048, name
                assert enhanced.width > original.width and enhanced.width * original.height == enhanced.height * original.width, name
        for folder in ('fonts', 'ui'):
            index = (hd / folder / 'index.txt').read_text().splitlines()
            assert len(index) > 1, (mode, folder)
        report[mode] = dict(profile=profile, pictures=len(expected), manifestFiles=len(listed), verified=True)
        print(f'PASS: {mode}: {len(expected)} HD pictures; complete pack hashes, dimensions, fonts and UI verified.', flush=True)
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
