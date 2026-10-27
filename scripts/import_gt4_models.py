"""Build a separate GT2 render-only model set from an owned GT4 US 1.01 ISO.

Python 3.11+, .NET SDK 8, internet for the first source-only tool build.
Existing game files and existing output directories are never overwritten.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request
import zipfile
import zlib

from gt4_iso import inventory, read_at, u32
from gt4_mesh import convert
from gt4_tool_patch import patch_source, REVISION as ADAPTER_REVISION

TOOL_COMMIT = '8bceeceedea8244536eb6106c3fb9da54557ceec'
PDTOOLS_COMMIT = '72d509a1482d87ba9aa5528cb7322e185679f38f'


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def source_archive(cache, repo, commit, expected):
    archive = cache / (repo + '-' + commit + '.zip')
    if not archive.exists():
        request = urllib.request.Request(f'https://api.github.com/repos/Nenkai/{repo}/zipball/{commit}',
                                         headers={'User-Agent': 'GT2-GT4-source-builder'})
        temporary = archive.with_suffix('.download')
        with urllib.request.urlopen(request, timeout=120) as response, temporary.open('wb') as output:
            shutil.copyfileobj(response, output)
        temporary.rename(archive)
    if digest(archive) != expected:
        raise ValueError(f'Source archive hash mismatch: {archive}. No code was executed.')
    destination = cache / (repo + '-' + commit)
    if not destination.exists():
        destination.mkdir()
        with zipfile.ZipFile(archive) as source:
            for member in source.infolist():
                target = (destination / member.filename).resolve()
                if not target.is_relative_to(destination.resolve()):
                    raise ValueError('Source archive contains an unsafe path')
            source.extractall(destination)
    roots = [p for p in destination.iterdir() if p.is_dir()]
    if len(roots) != 1:
        raise ValueError('Unexpected source archive layout')
    return roots[0]


def build_tool(cache):
    cache.mkdir(parents=True, exist_ok=True)
    binary = cache / 'bin' / 'GTPS2ModelTool.dll'
    stamp = cache / 'build.json'
    identity = {'modelTool': TOOL_COMMIT, 'pdtools': PDTOOLS_COMMIT, 'adapterRevision': ADAPTER_REVISION}
    if binary.exists() and stamp.exists() and json.loads(stamp.read_text()) == identity:
        return binary
    if not shutil.which('dotnet'):
        raise ValueError('Install .NET SDK 8, or pass --tool pointing to your locally built GTPS2ModelTool')
    print('Building pinned GTPS2ModelTool source; first run requires GitHub and NuGet.', flush=True)
    source = source_archive(cache, 'GTPS2ModelTool', TOOL_COMMIT,
                            'bba02cb8f935a568d9dde79aa6ded300810aef132c8f202d6c618ab691831db4')
    pdtools = source_archive(cache, 'PDTools', PDTOOLS_COMMIT,
                             '523b4c36580a6a823aef0593856ee8c998a17e7020b22d67ca6181dbe9f153e1')
    shutil.copytree(pdtools, source / 'PDTools', dirs_exist_ok=True)
    patch_source(source)
    subprocess.run(['dotnet', 'build', str(source / 'GTPS2ModelTool/GTPS2ModelTool.csproj'),
                    '-c', 'Release', '-o', str(binary.parent), '-p:NuGetAudit=false'], check=True)
    if not binary.exists():
        raise ValueError('Source build did not produce the model tool')
    stamp.write_text(json.dumps(identity, indent=2))
    return binary


def extract(stream, entry):
    if entry['size'] > 64 * 1024 * 1024 or entry['stored_size'] > 64 * 1024 * 1024:
        raise ValueError('Unexpected car resource size')
    data = read_at(stream, entry['offset'], entry['stored_size'])
    if entry['kind'] == 2:
        if data[:4] != bytes.fromhex('c5eef7ff') or ((-u32(data, 4)) & 0xffffffff) != entry['size']:
            raise ValueError('Invalid compressed car header')
        inflater = zlib.decompressobj(-15)
        data = inflater.decompress(data[8:], entry['size'] + 1)
        if inflater.unconsumed_tail:
            raise ValueError('Car resource exceeds declared size')
    if len(data) != entry['size']:
        raise ValueError('Car resource size mismatch')
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path, help='New separate model set directory')
    parser.add_argument('--tool', type=Path, help='Existing source-built GTPS2ModelTool.exe or .dll')
    parser.add_argument('--tool-cache', type=Path, default=Path(__file__).resolve().parents[1] / 'work/gt4-tools')
    parser.add_argument('--cars', nargs='+', help='GT2 car ids; default: all verified profiles')
    parser.add_argument('--profiles', type=Path, default=Path(__file__).with_name('gt4_profiles.json'),
                        help='Reviewed import profiles; defaults to the bundled set')
    parser.add_argument('--hd-interiors', type=Path, help='Optional HD Interiors RAR: record provenance and compatibility')
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists() or output.with_name(output.name + '.importing').exists():
        parser.error('Output or its .importing directory already exists. Choose a new output directory.')
    profiles = json.loads(args.profiles.read_text(encoding='utf-8-sig'))
    cars = args.cars or list(profiles['cars'])
    if len(set(cars)) != len(cars) or any(car not in profiles['cars'] for car in cars):
        parser.error('Unknown or repeated GT2 car id. Supported: ' + ', '.join(profiles['cars']))
    with args.iso.open('rb') as stream:
        catalog = inventory(stream)
        if catalog['toc_fingerprint_sha256'] != profiles['discTocSha256']:
            raise ValueError('This disc revision is not verified. Expected GT4 SCUS-97328 US 1.01; no output written.')
        entries = {x['path']: x for x in catalog['entries'] if x['kind']}
        # Validate all required records before building tools or writing assets.
        for car in cars:
            profile = profiles['cars'][car]
            if f"car/{profile['directory']}/{profile['code']}" not in entries:
                raise ValueError('Missing GT4 car: ' + profile['code'])
        if args.hd_interiors and not args.hd_interiors.is_file():
            raise ValueError('HD Interiors archive does not exist')
        tool = (args.tool or build_tool(args.tool_cache.resolve())).resolve()
        if not tool.is_file():
            raise ValueError('Model tool does not exist: ' + str(tool))
        command = ['dotnet', str(tool)] if tool.suffix.lower() == '.dll' else [str(tool)]
        staging = output.with_name(output.name + '.importing')
        staging.mkdir(parents=True)
        working = staging / '_source'; working.mkdir()
        provenance = {'iso': str(args.iso.resolve()), 'isoSize': args.iso.stat().st_size,
                      'tocSha256': catalog['toc_fingerprint_sha256'],
                      'toolCommit': TOOL_COMMIT, 'pdtoolsCommit': PDTOOLS_COMMIT,
                      'adapterRevision': ADAPTER_REVISION if not args.tool else 'external-tool',
                      'toolBinarySha256': digest(tool), 'models': [], 'files': {}}
        provenance['profilesSha256'] = digest(args.profiles)
        manifest = {'format': 'gt2-model-set', 'version': 1, 'id': 'gt4', 'cars': {}}
        dumped = {}
        for car in cars:
            profile = profiles['cars'][car]; code = profile['code']
            archive_path = f"car/{profile['directory']}/{code}"
            print(f"Converting {car}: {profile['name']} ({code})", flush=True)
            # The same donor can serve several GT2 trims. Decode it once per
            # resource path; open/lod versions must never share a dump directory.
            if archive_path not in dumped:
                donor = working / profile['directory']; donor.mkdir(exist_ok=True)
                raw = donor / code; raw.write_bytes(extract(stream, entries[archive_path]))
                with (donor / (code + '.log')).open('w', encoding='utf8') as log:
                    subprocess.run(command + ['dump', '-i', str(raw)], stdout=log,
                                   stderr=subprocess.STDOUT, check=True, timeout=120)
                dumped[archive_path] = (donor / (code + '_dump'), digest(raw))
            dump, raw_sha256 = dumped[archive_path]
            if not (dump/'texture_addressing.json').exists():
                raise ValueError('Model tool omits TEX0/CLAMP metadata; rebuild with adapter revision 11')
            skips = [f'0.{i}.png' for i in profile.get('cockpitSkipTextures', [])]
            stats = {name: convert(dump, staging / car / filename, cockpit=cockpit,
                                    cockpit_skip_textures=skips,
                                    include_shapes=(profile.get('cockpitIncludeShapes', profile.get('includeShapes', []) if profile.get('nativeInterior') else [])
                                                    if cockpit else profile.get('includeShapes', [])),
                                    opaque_textures=[f'0.{i}.png' for i in profile.get('opaqueTextures', [])],
                                    mirror_textures=[f'0.{i}.png' for i in profile.get('mirrorTextures', [])])
                     for name, filename, cockpit in [('exterior','body.gltf',False),('cockpit','cockpit.gltf',True)]}
            if 3 * sum(s['triangles'] for s in stats.values()) > 65536:
                raise ValueError('Car exceeds runtime vertex budget: ' + car)
            (staging / car / 'conversion.json').write_text(json.dumps(stats, indent=2))
            entry = {'name':profile['name'], 'gt4Model':code, 'mesh':car+'/body.gltf',
                     'cockpitMesh':car+'/cockpit.gltf', 'nativeInterior':profile.get('nativeInterior',False)}
            for field in ('referenceName', 'matchType', 'matchNote'):
                if field in profile: entry[field] = profile[field]
            if entry['nativeInterior']:
                entry.update(eye=profile['eye'], mirror=profile['mirror'])
                if 'speedometer' in profile: entry['speedometer']=profile['speedometer']
            manifest['cars'][car] = entry
            provenance['models'].append({'gt2':car, 'path':archive_path, 'sha256':raw_sha256})
        if args.hd_interiors:
            provenance['hdInteriors'] = {'path':str(args.hd_interiors.resolve()), 'sha256':digest(args.hd_interiors),
                'appliedTextures':0, 'reason':'Pack 1 targets other cars. No verified texture mapping for these profiles.'}
            print('HD Interiors Pack 1: recorded, 0 matching profiles; textures were not applied.', flush=True)
        for path in sorted(staging.rglob('*')):
            if path.is_file() and working not in path.parents:
                provenance['files'][path.relative_to(staging).as_posix()] = digest(path)
        (staging / 'provenance.json').write_text(json.dumps(provenance, indent=2), encoding='utf8')
        # Publish only after every car is converted. Failed runs retain .importing
        # for diagnosis and never advertise a complete manifest.
        (staging / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf8')
        staging.rename(output)
    print(f'GT4 model set ready: {output}\nCars: {len(cars)}. Set GT2_GT4_MODEL_SET to this directory.', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        print('Import failed: ' + str(error), file=sys.stderr)
        sys.exit(1)
