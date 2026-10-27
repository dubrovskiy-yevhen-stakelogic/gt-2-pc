"""Package a complete 0.9.0 GT4 model set for desktop or Quest installation.

The bundle contains only model assets and optional catalogue expansions. Saves,
settings, original disc files and HD menus are never included or replaced.
Diagnostic subsets are intentionally rejected, including model-only bundles.
"""
import argparse
import gzip
import hashlib
import io
import json
import os
import posixpath
from pathlib import Path, PurePosixPath
import re
import tarfile


def document(path):
    data = path.read_bytes()
    return json.loads(gzip.decompress(data) if path.suffix == '.gz' else data)


def allowed(name):
    return bool(re.fullmatch(r'(modelsets/gt4|arcade/expansions/gt4|simulation/expansions/gt4)(/[A-Za-z0-9_.-]+)+', name)) and '..' not in PurePosixPath(name).parts and PurePosixPath(name).as_posix() == name


def validate_catalogue(names, read_bytes):
    """Validate active catalogue references as well as the checksum inventory."""
    def data(name):
        if name not in names:
            raise ValueError('Missing required GT4 asset: ' + name)
        return read_bytes(name)

    def doc(name):
        raw = data(name)
        return json.loads(gzip.decompress(raw) if name.endswith('.gz') else raw)

    def reference(base, uri):
        if not isinstance(uri, str) or '\\' in uri or ':' in uri or uri.startswith('/'):
            raise ValueError('Invalid GT4 asset reference')
        name = posixpath.normpath(posixpath.join(base, uri))
        if not allowed(name) or not name.startswith('modelsets/gt4/') or name not in names:
            raise ValueError('Missing or unsafe GT4 asset reference: ' + name)
        return name

    root = 'modelsets/gt4/'
    native_root = root + 'native-v16/'
    manifest = doc(root + 'manifest.json')
    overlay = doc(native_root + 'manifest.json')
    cabins = doc(native_root + 'interiors.json')
    wheels = doc(native_root + 'wheels.json')
    if (manifest.get('format'), manifest.get('version'), manifest.get('id')) != ('gt2-model-set', 1, 'gt4'):
        raise ValueError('Unsupported model set')
    cars = manifest.get('cars', {})
    if len(cars) != 1110 or any(not re.fullmatch('[a-z0-9-]{5}', car) for car in cars):
        raise ValueError('Full GT4 bundle requires 1110 model mappings; diagnostic subsets cannot be installed')
    if (overlay.get('format'), overlay.get('version')) != ('gt4-native-overlay', 1) or set(overlay.get('cars', {})) != set(cars):
        raise ValueError('Native overlay must cover the complete model catalogue')
    if (cabins.get('format'), cabins.get('version')) != ('gt4-native-interiors', 1):
        raise ValueError('Unsupported interior metadata')
    for car, row in cabins.get('cars', {}).items():
        if car not in cars or row.get('mesh') != overlay['cars'][car]:
            raise ValueError('Interior source differs from selected model: ' + car)
    authored = sum(bool({**entry, **cabins.get('cars', {}).get(car, {})}.get('nativeInterior'))
                   for car, entry in cars.items())
    if authored != 89:
        raise ValueError('Full GT4 bundle requires 89 authored-interior mappings')
    if (wheels.get('format'), wheels.get('version')) != ('gt4-wheel-geometry', 2):
        raise ValueError('Unsupported wheel geometry')
    if not wheels.get('cars') or not set(wheels['cars']) <= set(cars):
        raise ValueError('Wheel geometry differs from the model catalogue')
    missing = set(wheels.get('missing', []))
    if any(entry.get('gt4Model') not in missing for car, entry in cars.items() if car not in wheels['cars']):
        raise ValueError('Wheel geometry has undeclared missing models')
    required = {car for car, entry in cars.items() if entry.get('required')}
    if len(required) != 722:
        raise ValueError('Full GT4 bundle requires 722 added catalogue models')
    for mode in ('arcade', 'simulation'):
        prefix = mode + '/expansions/gt4/'
        if not any(name.startswith(prefix) for name in names):
            continue
        if data(prefix + 'catalogue.version').strip() != b'gt2-car-expansion-1':
            raise ValueError('Unsupported catalogue expansion')
        expansion = doc(prefix + 'assets/carparam/gt4_expansion.json')
        entries = expansion.get('cars', [])
        ids = [entry['id'] for entry in entries]
        if ((expansion.get('format'), expansion.get('version')) != ('gt2-car-expansion', 1)
                or len(ids) != 722 or len(set(ids)) != len(ids) or set(ids) != required):
            raise ValueError('Catalogue expansion does not match the required GT4 models: ' + mode)
    for mesh in set(overlay['cars'].values()):
        mesh_name = reference(native_root, mesh)
        base = posixpath.dirname(mesh_name)
        for kind in ('buffers', 'images'):
            for asset in doc(mesh_name).get(kind, []):
                reference(base, asset['uri'])
        palette = doc(base + '/paints.json.gz')
        if (palette.get('format'), palette.get('version')) != ('gt4-native-paints', 1) or not palette.get('colors'):
            raise ValueError('Invalid native paint palette: ' + base)
        for color in palette['colors']:
            for image in color.get('images', {}).values():
                reference(native_root, image)


def verify(path):
    seen = set()
    entries = {}
    with tarfile.open(path, 'r:') as archive:
        for entry in archive:
            if not entry.isfile() or not allowed(entry.name) or entry.name in seen:
                raise ValueError('Unsafe or duplicate bundle entry: ' + entry.name)
            seen.add(entry.name)
            entries[entry.name] = entry
        required = {'modelsets/gt4/manifest.json', 'modelsets/gt4/native-v16/manifest.json',
                    'modelsets/gt4/native-v16/wheels.json', 'modelsets/gt4/quest-install.sha256'}
        if not required <= seen:
            raise ValueError('Incomplete GT4 bundle')
        inventory = archive.extractfile(entries['modelsets/gt4/quest-install.sha256']).read().decode('ascii')
        expected = {}
        for line in inventory.splitlines():
            digest, name = line.split('  ', 1)
            if not re.fullmatch('[0-9a-f]{64}', digest) or not allowed(name) or name in expected:
                raise ValueError('Invalid bundle checksum inventory')
            expected[name] = digest
        if set(expected) != seen - {'modelsets/gt4/quest-install.sha256'}:
            raise ValueError('Bundle inventory differs from its contents')
        for name, digest in expected.items():
            with archive.extractfile(entries[name]) as stream:
                if hashlib.file_digest(stream, 'sha256').hexdigest() != digest:
                    raise ValueError('Bundle checksum mismatch: ' + name)
        validate_catalogue(seen, lambda name: archive.extractfile(entries[name]).read())
    print(f'GT4 bundle verified: {len(expected)} files.', flush=True)


def build(args):
    pack, native = args.pack.absolute(), args.native.absolute()
    manifest, overlay = document(pack/'manifest.json'), document(native/'manifest.json')
    if manifest.get('format') != 'gt2-model-set' or manifest.get('version') != 1 or manifest.get('id') != 'gt4':
        raise ValueError('Unsupported model set')
    if overlay.get('format') != 'gt4-native-overlay' or overlay.get('version') != 1 or not overlay.get('cars'):
        raise ValueError('Unsupported native overlay')
    if set(manifest['cars']) != set(overlay['cars']):
        raise ValueError('Native overlay must cover the complete model catalogue')
    files = {}
    def add(path, name):
        if not allowed(name) or not path.is_file():
            raise ValueError('Invalid model asset: ' + str(path))
        if name in files and files[name] != path:
            raise ValueError('Conflicting model asset: ' + name)
        files[name] = path
    def native_asset(path):
        # Follow explicit glTF/palette references, never recursively traverse
        # source junctions. Some textures are beside the native overlay.
        path = Path(os.path.abspath(path))
        relative = ('native-v16/' + path.relative_to(native).as_posix()
                    if path.is_relative_to(native) else path.relative_to(native.parent).as_posix())
        add(path, 'modelsets/gt4/' + relative)
    add(pack/'manifest.json', 'modelsets/gt4/manifest.json')
    native_asset(native/'manifest.json')
    native_asset(native/'wheels.json')
    if (native/'interiors.json').is_file():
        cabins=document(native/'interiors.json')
        if cabins.get('format')!='gt4-native-interiors' or cabins.get('version')!=1:raise ValueError('Unsupported interior metadata')
        for key,row in cabins['cars'].items():
            if row.get('mesh')!=overlay['cars'].get(key):raise ValueError('Interior source differs from selected model: '+key)
        native_asset(native/'interiors.json')
    for name in set(overlay['cars'].values()):
        if Path(name).is_absolute() or ':' in name or '..' in Path(name).parts:
            raise ValueError('Unsafe native model path')
        mesh = native/name
        native_asset(mesh)
        for kind in ('buffers', 'images'):
            for entry in document(mesh).get(kind, []):
                native_asset(mesh.parent/entry['uri'])
        palette = mesh.parent/'paints.json.gz'
        native_asset(palette)
        for color in document(palette)['colors']:
            for image in color['images'].values():
                native_asset(native/image)
    for mode in ('arcade', 'simulation'):
        expansion = getattr(args, mode + '_expansion')
        if expansion is None:
            continue
        expansion = expansion.absolute()
        if (expansion/'catalogue.version').read_text().strip() != 'gt2-car-expansion-1':
            raise ValueError('Unsupported catalogue expansion')
        add(expansion/'catalogue.version', mode+'/expansions/gt4/catalogue.version')
        for directory, children, names in os.walk(expansion/'assets', followlinks=False):
            for name in children + names:
                child = Path(directory)/name
                if child.is_symlink() or (getattr(child.stat(), 'st_file_attributes', 0) & 0x400):
                    raise ValueError('Expansion links require explicit asset references')
            for name in names:
                path = Path(directory)/name
                add(path, mode+'/expansions/gt4/'+path.relative_to(expansion).as_posix())
    validate_catalogue(set(files), lambda name: files[name].read_bytes())
    if args.output.exists():
        raise ValueError('Bundle already exists; choose a new output path')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    inventory = []
    with tarfile.open(args.output, 'x', format=tarfile.USTAR_FORMAT) as archive:
        for count, (name, path) in enumerate(sorted(files.items()), 1):
            data = path.read_bytes()
            inventory.append(hashlib.sha256(data).hexdigest() + '  ' + name)
            entry = tarfile.TarInfo(name)
            entry.size, entry.mode = len(data), 0o644
            archive.addfile(entry, io.BytesIO(data))
            if count % 10000 == 0:
                print(f'GT4 bundle: {count}/{len(files)} files.', flush=True)
        data = ('\n'.join(inventory)+'\n').encode('ascii')
        entry = tarfile.TarInfo('modelsets/gt4/quest-install.sha256')
        entry.size, entry.mode = len(data), 0o644
        archive.addfile(entry, io.BytesIO(data))
    verify(args.output)
    print(f'Prepared {len(manifest["cars"])} GT4 mappings: {args.output}', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify', type=Path)
    for option in ('pack', 'native', 'arcade-expansion', 'simulation-expansion', 'output'):
        parser.add_argument('--' + option, type=Path)
    args = parser.parse_args()
    if args.verify:
        verify(args.verify)
    elif args.pack and args.native and args.output:
        build(args)
    else:
        parser.error('--pack, --native and --output are required')
