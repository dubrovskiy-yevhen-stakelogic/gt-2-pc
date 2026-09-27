#!/usr/bin/env python3
"""Prepare installed GT2 media with the native tools and Real-ESRGAN."""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import urllib.request
import uuid
import zipfile

UPSCALER_URL = ('https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/'
                'realesrgan-ncnn-vulkan-20220424-macos.zip')
UPSCALER_BYTES = 51817124
MODEL = 'realesrgan-x4plus'


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def run(*args, capture=False):
    return subprocess.run([str(a) for a in args], check=True, text=True,
                          stdout=subprocess.PIPE if capture else None).stdout


@contextmanager
def runtime_lock(root):
    import fcntl
    path = root / '.macos-app.lock'
    fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError('Close GT2 and its disc importer before preparing HD media.') from None
        yield
    finally:
        os.close(fd)


def extract_tool(archive, folder):
    with zipfile.ZipFile(archive) as package:
        for entry in package.infolist():
            name = PurePosixPath(entry.filename)
            if (name.is_absolute() or '..' in name.parts or '\\' in entry.filename or
                    ':' in entry.filename or stat.S_ISLNK(entry.external_attr >> 16)):
                raise ValueError('Unsafe path in Real-ESRGAN archive')
        if package.testzip() is not None:
            raise ValueError('Damaged Real-ESRGAN archive')
        package.extractall(folder)
    matches = list(folder.rglob('realesrgan-ncnn-vulkan'))
    matches = [p for p in matches if p.is_file() and '__MACOSX' not in p.parts]
    if len(matches) != 1:
        raise ValueError('Real-ESRGAN executable was not found in the archive')
    executable = matches[0]
    for suffix in ('.param', '.bin'):
        if not (executable.parent / 'models' / (MODEL + suffix)).is_file():
            raise ValueError('Real-ESRGAN x4plus model is missing')
    executable.chmod(0o755)
    return executable


def obtain_upscaler(cache, local=None):
    if local:
        executable = local.expanduser().resolve(strict=True)
        for suffix in ('.param', '.bin'):
            if not (executable.parent / 'models' / (MODEL + suffix)).is_file():
                raise ValueError('Put the x4plus models beside the supplied Real-ESRGAN executable')
        return executable
    archive = cache / 'realesrgan-20220424-macos.zip'
    receipt = archive.with_suffix('.json')
    verified = False
    if archive.is_file() and receipt.is_file():
        record = json.loads(receipt.read_text())
        verified = record.get('url') == UPSCALER_URL and record.get('sha256') == digest(archive)
    if not verified:
        print('Downloading the official macOS Real-ESRGAN package (52 MB).', flush=True)
        temporary = cache / ('download-' + uuid.uuid4().hex + '.zip')
        try:
            with urllib.request.urlopen(UPSCALER_URL, timeout=60) as response, temporary.open('xb') as output:
                if not response.url.startswith('https://'):
                    raise ValueError('The download redirected away from HTTPS')
                shutil.copyfileobj(response, output)
            if temporary.stat().st_size != UPSCALER_BYTES:
                raise ValueError('The upstream Real-ESRGAN archive changed; supply --upscaler instead')
            with zipfile.ZipFile(temporary) as package:
                if package.testzip() is not None:
                    raise ValueError('Damaged Real-ESRGAN download')
            temporary.replace(archive)
            receipt.write_text(json.dumps({'url': UPSCALER_URL, 'sha256': digest(archive)}, indent=2) + '\n')
        finally:
            temporary.unlink(missing_ok=True)
    folder = Path(tempfile.mkdtemp(prefix='realesrgan-', dir=cache))
    return extract_tool(archive, folder)


def validate_index(folder, header):
    lines = (folder / 'index.txt').read_text(encoding='utf-8-sig').splitlines()
    if not lines or lines[0].strip() != header or len(lines) < 2:
        raise ValueError('Invalid HD index: ' + str(folder))
    pattern = r'[0-9a-f]{16}-[0-9a-f]{4}' if header == 'palette-contours4x-v1' else r'[0-9a-f]{16}'
    for key in lines[1:]:
        if not re.fullmatch(pattern, key) or not (folder / (key + '.png')).is_file():
            raise ValueError('Incomplete HD atlas: ' + str(folder))


def manifest(folder, profile):
    files = [{'path': p.relative_to(folder).as_posix(), 'sha256': digest(p)}
             for p in sorted(folder.rglob('*')) if p.is_file() and p.name != 'manifest.json']
    (folder / 'manifest.json').write_text(json.dumps({
        'format': 1, 'profile': profile, 'model': MODEL, 'scale': 4,
        'ui': 'indexed-scale4x-v2', 'fonts': 'palette-contours4x-v1',
        'video': 'source-bounded-v1', 'files': files}, indent=2) + '\n')


def publish(stage, destination):
    if destination.is_symlink():
        raise ValueError('Refusing to replace a linked HD directory')
    backup = destination.with_name('hd.backup-' + uuid.uuid4().hex)
    had_old = destination.exists()
    if had_old:
        destination.rename(backup)
    try:
        stage.rename(destination)
    except BaseException:
        if had_old:
            backup.rename(destination)
        raise
    if had_old:
        print('Previous HD pack:', backup)


def prepare_disc(root, mode, tools, upscale, media_mode, gpu):
    media, inspect = tools
    disc = root / mode / 'disc.raw2352'
    info = json.loads(run(inspect, 'inspect', disc, capture=True))
    profile = info['exeSha1']
    cache = root / '.hd-work' / (digest(disc) + '-mac-v1')
    cache.mkdir(parents=True, exist_ok=True)
    destination = root / mode / 'hd'
    if destination.is_symlink():
        raise ValueError('Refusing to replace a linked HD directory')
    # Keep preparation and publication on the disc directory's filesystem.
    stage = Path(tempfile.mkdtemp(prefix='.hd-candidate-', dir=root / mode))
    print('Preparing', mode, flush=True)

    def enlarge(source, output):
        output.mkdir(parents=True, exist_ok=True)
        args = [upscale, '-i', source, '-o', output, '-m', upscale.parent / 'models',
                '-n', MODEL, '-s', '4', '-t', '128', '-f', 'png']
        if gpu is not None:
            args += ['-g', gpu]
        run(*args)

    original = cache / 'images-original'
    run(media, 'images', disc, original)
    if (original / 'profile.txt').read_text().strip() != profile:
        raise ValueError('Extracted HD profile does not match the installed disc')
    inputs = cache / 'image-input'
    inputs.mkdir(exist_ok=True)
    expected = {p.name for p in original.glob('*.png')}
    if not expected:
        raise ValueError('No pictures extracted')
    for name in expected:
        shutil.copy2(original / name, inputs / name)
    images = cache / 'images-upscaled'
    stamp = images / 'complete.txt'
    if not stamp.is_file() or {p.name for p in images.glob('*.png')} != expected:
        enlarge(inputs, images)
        run(media, 'fit-images', images)
        if {p.name for p in images.glob('*.png')} != expected:
            raise ValueError('Incomplete upscaled picture set')
        stamp.write_text(MODEL + '\n')
    (stage / 'images').mkdir()
    for name in expected:
        shutil.copy2(images / name, stage / 'images' / name)
    (stage / 'profile.txt').write_text(profile + '\n')
    for command, folder, header in [('ui', 'ui', 'indexed-scale4x-v2'),
                                     ('fonts', 'fonts', 'palette-contours4x-v1')]:
        output = cache / folder
        stamp = cache / (folder + '.complete')
        if not stamp.is_file():
            run(media, command, disc, output)
            if command == 'ui':
                run(media, 'ui-maps', disc, output)
            validate_index(output, header)
            stamp.write_text('complete\n')
        validate_index(output, header)
        shutil.copytree(output, stage / folder)
    if destination.is_dir():
        old_profile = destination / 'profile.txt'
        if not old_profile.is_file() or old_profile.read_text(encoding='utf-8-sig').strip() != profile:
            raise ValueError('Existing HD pack belongs to another disc')
        for name in ('movies', 'startup.gtm', 'startup-hd.gtm'):
            source = destination / name
            if source.is_dir():
                shutil.copytree(source, stage / name)
            elif source.is_file():
                shutil.copy2(source, stage / name)
    if media_mode == 'MenusAndMovies' and mode == 'arcade':
        (stage / 'movies').mkdir(exist_ok=True)
        for movie_id in (24, 25, 26):
            packed = cache / (str(movie_id) + '-source-bounded-v1.gtm')
            if not packed.is_file():
                frames = cache / ('movie-' + str(movie_id))
                if not (frames / 'movie.txt').is_file():
                    run(media, 'movie', disc, movie_id, frames)
                movie_inputs = cache / ('movie-input-' + str(movie_id))
                movie_inputs.mkdir(exist_ok=True)
                for frame in frames.glob('*.png'):
                    target = movie_inputs / frame.name
                    if not target.exists():
                        try:
                            os.link(frame, target)
                        except OSError:
                            shutil.copy2(frame, target)
                enhanced = cache / ('movie-upscaled-' + str(movie_id))
                enlarge(movie_inputs, enhanced)
                for name in ('movie.txt', 'audio.pcm'):
                    shutil.copy2(frames / name, enhanced / name)
                candidate = packed.with_suffix('.candidate')
                run(media, 'pack', enhanced, candidate, frames)
                run(media, 'inspect', candidate)
                candidate.replace(packed)
            run(media, 'inspect', packed)
            shutil.copy2(packed, stage / 'movies' / (str(movie_id) + '.gtm'))
    manifest(stage, profile)
    publish(stage, destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, default=Path(os.environ.get(
        'GT2_DATA_ROOT', str(Path.home() / 'Library/Application Support/GT2'))))
    parser.add_argument('--tools', type=Path, help='Directory containing gt2media and gt2install')
    parser.add_argument('--upscaler', type=Path, help='Local Real-ESRGAN executable with models beside it')
    parser.add_argument('--media', choices=('Menus', 'MenusAndMovies'))
    parser.add_argument('--gpu', type=int, help='Real-ESRGAN device ID; -1 selects CPU')
    args = parser.parse_args()
    if platform.system() != 'Darwin':
        parser.error('This command requires macOS')
    root = args.runtime.expanduser().resolve(strict=True)
    if any((root / mode).is_symlink() for mode in ('arcade', 'simulation')):
        raise ValueError('Linked disc directories are not supported')
    modes = [mode for mode in ('arcade', 'simulation') if (root / mode / 'disc.raw2352').is_file()]
    if not modes:
        raise ValueError('Import your original discs in GT2.app first')
    candidates = [args.tools] if args.tools else [
        Path.home() / 'Applications/GT2.app/Contents/MacOS', Path('/Applications/GT2.app/Contents/MacOS'),
        Path(__file__).resolve().parents[1] / 'build_macos']
    tools = next((p for p in candidates if p and all(os.access(p / name, os.X_OK)
                 for name in ('gt2media', 'gt2install'))), None)
    if tools is None:
        raise ValueError('Run the 0.6.0 release INSTALL-MACOS.command first; older test apps omit gt2media')
    if not args.media:
        print('1. HD pictures, menu text and HUD\n2. Also prepare full-screen movies (slow; needs tens of GB)')
        choice = input('Choose 1 or 2 (Enter = 1): ').strip()
        if choice not in ('', '1', '2'):
            raise ValueError('Choose 1 or 2')
        args.media = 'MenusAndMovies' if choice == '2' else 'Menus'
    with runtime_lock(root):
        cache = root / '.hd-work' / 'tools'
        cache.mkdir(parents=True, exist_ok=True)
        upscale = obtain_upscaler(cache, args.upscaler)
        for mode in modes:
            prepare_disc(root, mode, (tools / 'gt2media', tools / 'gt2install'), upscale, args.media, args.gpu)
    print('HD media is ready. Enable HD textures and media in Shift+Q > Graphics and performance.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError, zipfile.BadZipFile, KeyboardInterrupt) as error:
        sys.exit(str(error) or 'HD preparation interrupted')
