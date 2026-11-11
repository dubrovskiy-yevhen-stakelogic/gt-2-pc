"""Combine same-run Windows tools and signed Quest APK into one installer ZIP."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import tempfile
import zipfile


def package(windows, apk, output):
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        with zipfile.ZipFile(windows) as archive:
            for entry in archive.infolist():
                parts = PurePosixPath(entry.filename).parts
                if entry.is_dir():
                    continue
                if len(parts) < 2 or any(p in ('..', '/') or ':' in p or '\\' in p for p in parts):
                    raise ValueError('Unsafe archive path')
                dest = root.joinpath(*parts[1:])
                if dest.exists():
                    raise ValueError('Duplicate archive path')
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(archive.read(entry))
        manifest = json.loads((root / 'release-manifest.json').read_text(encoding='utf-8-sig'))
        version = manifest['version']
        import re
        if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', version):
            raise ValueError('Invalid version')
        if apk.name != f'GT2-VR-{version}.apk':
            raise ValueError('Windows and APK versions differ')
        for entry in manifest['files']:
            path = PurePosixPath(entry['path'])
            if path.is_absolute() or '..' in path.parts or ':' in str(path) or '\\' in str(path):
                raise ValueError('Unsafe manifest path')
            if hashlib.sha256((root / path).read_bytes()).hexdigest() != entry['sha256'].lower():
                raise ValueError('Windows package checksum mismatch: ' + str(path))
        (root / apk.name).write_bytes(apk.read_bytes())
        launcher = (root / 'INSTALL-PCVR.bat').read_text()
        if '-Target PCVR' not in launcher:
            raise ValueError('PCVR installer entry point missing')
        for name in ('INSTALL.bat', 'INSTALL-QUEST.bat'):
            (root / name).write_text(launcher.replace('-Target PCVR', '-Target Quest'), encoding='ascii')
        (root / 'START-HERE.txt').write_text(
            'Extract this archive on Windows. Quest: connect by USB, accept USB debugging, '
            'then run INSTALL-QUEST.bat (or INSTALL.bat). PCVR: run INSTALL-PCVR.bat.\n'
            'Select your own GT2 disc image. The installer imports data and installs the bundled APK.\n'
            'No source kit, compiler or separate APK download needed. Internet is required for prerequisites.\n')
        manifest['files'] = [dict(path=p.relative_to(root).as_posix(), bytes=p.stat().st_size,
            sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in sorted(root.rglob('*'))
            if p.is_file() and p.name != 'release-manifest.json']
        (root / 'release-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        output.mkdir(parents=True, exist_ok=True)
        name = f'GT2-VR-{version}'
        target = output / (name + '.zip')
        with zipfile.ZipFile(target, 'x', zipfile.ZIP_DEFLATED) as archive:
            for p in sorted(root.rglob('*')):
                if p.is_file():
                    archive.write(p, name + '/' + p.relative_to(root).as_posix())
        digest = hashlib.sha256(target.read_bytes()).hexdigest()
        (output / 'SHA256SUMS-vr.txt').write_text(digest + '  ' + target.name + '\n')
        return target


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--windows', type=Path, required=True)
    parser.add_argument('--apk', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(package(args.windows, args.apk, args.output))
