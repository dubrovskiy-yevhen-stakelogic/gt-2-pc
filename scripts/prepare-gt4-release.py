"""Build the complete GT4 expansion from owned GT4 and GT2 disc images.

Build the bundled native tools first. Model conversion uses the pinned .NET
model-tool source; --offline requires an already built tool. The output is a new
directory, and no installed game, disc image or save is modified.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from gt4_iso import inventory, read_at
from gt4_mesh import convert
from import_gt4_models import extract, build_tool, TOOL_COMMIT

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def relocate_evidence(output, previous):
    """Keep the retained raw-input provenance usable after publishing a run."""
    prefixes = (str(previous), previous.as_posix())
    def relocated(value):
        if isinstance(value, dict):
            return {k: relocated(v) for k, v in value.items()}
        if isinstance(value, list):
            return [relocated(v) for v in value]
        if isinstance(value, str):
            for prefix in prefixes:
                if value == prefix or value.startswith((prefix+'/', prefix+'\\')):
                    return str(output)+value[len(prefix):]
        return value
    for relative in ('alignment.json', 'alignment/report.json', 'paints/report.json',
                     'native/report.json', 'modelsets/gt4/provenance.json'):
        path = output/relative
        write(path, relocated(read(path)))


def finalize_evidence(output, previous):
    relocate_evidence(output, previous)
    # Wheel audit hashes bind the alignment JSON, including its source paths.
    # Re-run its generator after relocation rather than editing those hashes.
    script('build_gt4_wheels.py', ['--specdb',output/'specdb-json','--alignment',output/'native/report.json',
        '--showroom',output/'showroom.json','--output',output/'native/native-v16/wheels.json'],output/'logs/wheels-published.log')
    shutil.copyfile(output/'native/native-v16/wheels.json',output/'modelsets/gt4/native-v16/wheels.json')


def command(args, log, *, capture=False):
    log.parent.mkdir(parents=True, exist_ok=True)
    print('Running:', ' '.join(map(str, args[:3])), flush=True)
    with log.open('w', encoding='utf-8') as output:
        result = subprocess.run(list(map(str, args)), cwd=ROOT, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace')
        output.write(result.stdout)
    if result.returncode:
        raise RuntimeError(f'Command failed ({result.returncode}); see {log}')
    return result.stdout if capture else None


def script(name, args, log):
    return command([sys.executable, ROOT/'scripts'/name, *args], log)


def native_tool(build, name):
    for parent in [build, build/'Release']:
        candidate = parent/(name + ('.exe' if os.name == 'nt' else ''))
        if candidate.is_file():
            return candidate
    raise ValueError(f'Missing {name}; build the gt4-import-tools CMake target in {build}')


def model_command(tool):
    return ['dotnet', tool] if tool.suffix.lower() == '.dll' else [tool]


def extract_gt4(iso, vol, profiles, needed_codes=None):
    with iso.open('rb') as stream:
        index = inventory(stream)
        if index['toc_fingerprint_sha256'] != profiles['discTocSha256']:
            raise ValueError('Expected Gran Turismo 4 USA v1.01 (SCUS-97328)')
        from gt4_core import unpack, build_elf, KNOWN_CORE_SHA1
        core_entry = next(e for e in index['iso_root'] if e['name'] == 'CORE.GT4')
        core = read_at(stream, core_entry['lba']*2048, core_entry['size'])
        if hashlib.sha1(core).hexdigest() != KNOWN_CORE_SHA1:
            raise ValueError('Unverified GT4 CORE image')
        decoded = unpack(core)
        vol.mkdir(parents=True, exist_ok=True)
        (vol/'CORE.elf').write_bytes(build_elf(decoded['entry'], decoded['segments']))
        entries = {e['path']: e for e in index['entries'] if e['kind']}
        for path, entry in entries.items():
            if not path.startswith(('car/lod/', 'car/open/', 'car/menu/', 'specdb/GT4_US2560/')):
                continue
            if needed_codes is not None and path.startswith('car/') and Path(path).name.removesuffix('.pat') not in needed_codes:
                continue
            target = (vol/path).resolve()
            if not target.is_relative_to(vol.resolve()):
                raise ValueError('Unsafe source archive path')
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(extract(stream, entry))
    return dict(iso=iso.name, isoBytes=iso.stat().st_size, tocSha256=index['toc_fingerprint_sha256'])


def build_models(pack, vol, profiles, added, policies, tool, selected=None):
    """Reproduce replacement profiles, then add each named GT4 catalogue record."""
    rows = profiles['cars']
    manifest = dict(format='gt2-model-set', version=1, id='gt4', cars={})
    jobs = {}
    by_code = {}
    for car, profile in rows.items():
        entry = dict(name=profile['name'], gt4Model=profile['code'], mesh=car+'/body.gltf',
                     cockpitMesh=car+'/cockpit.gltf', nativeInterior=profile.get('nativeInterior', False))
        for field in ('referenceName', 'matchType', 'matchNote', 'eye', 'mirror', 'speedometer'):
            if field in profile:
                entry[field] = profile[field]
        manifest['cars'][car] = entry
        jobs[car] = dict(profile=profile, directory=profile['directory'], code=profile['code'])
        by_code[profile['code']] = entry
    new_codes = {}
    for car in added:
        car_id, code = car['id'], car['code']
        policy = policies.get(car_id, {})
        if code in by_code and not policy.get('directory'):
            entry = dict(by_code[code])
        elif code in new_codes and not policy.get('directory'):
            entry = dict(new_codes[code])
        else:
            parent = policy.get('parent', car_id)
            entry = dict(gt4Model=code, mesh=parent+'/body.gltf', cockpitMesh=parent+'/cockpit.gltf', nativeInterior=False)
            jobs[parent] = dict(profile={}, directory=policy.get('directory', 'lod'), code=code)
            new_codes[code] = entry
        entry.update(name=car['name'], required=True)
        entry.update({k: v for k, v in policy.items() if k not in ('directory', 'parent')})
        manifest['cars'][car_id] = entry
    if len(manifest['cars']) != 1110:
        raise ValueError('Expected 388 replacement mappings and 722 added cars')
    if selected:
        unknown = set(selected)-set(manifest['cars'])
        if unknown:
            raise ValueError('Unknown model IDs: '+', '.join(sorted(unknown)))
        manifest['cars'] = {k: v for k, v in manifest['cars'].items() if k in selected}
    active = {Path(e['mesh']).parent.as_posix() for e in manifest['cars'].values()}
    provenance = []
    for number, parent in enumerate(sorted(active), 1):
        job = jobs[parent]
        profile, code = job['profile'], job['code']
        raw = vol/'car'/job['directory']/code
        if not raw.is_file():
            raise ValueError('Missing CAR4 source: '+str(raw))
        dump = raw.with_name(code+'_dump')
        if not (dump/'texture_addressing.json').is_file():
            command([*model_command(tool), 'dump', '-i', raw], pack.parent/'logs'/f'obj-{parent}.log')
        if not (dump/'texture_addressing.json').is_file():
            raise ValueError('Model tool lacks TEX0/CLAMP metadata; rebuild the pinned adapter')
        stats = {}
        for part, file, seated in [('exterior', 'body.gltf', False), ('cockpit', 'cockpit.gltf', True)]:
            include = (profile.get('cockpitIncludeShapes', profile.get('includeShapes', []) if profile.get('nativeInterior') else [])
                       if seated else profile.get('includeShapes', []))
            stats[part] = convert(dump, pack/parent/file, cockpit=seated, include_shapes=include,
                cockpit_skip_textures=[f'0.{i}.png' for i in profile.get('cockpitSkipTextures', [])],
                opaque_textures=[f'0.{i}.png' for i in profile.get('opaqueTextures', [])],
                mirror_textures=[f'0.{i}.png' for i in profile.get('mirrorTextures', [])])
        if 3*sum(s['triangles'] for s in stats.values()) > 65536:
            raise ValueError('Model exceeds runtime vertex budget: '+parent)
        write(pack/parent/'conversion.json', stats)
        provenance.append(dict(mesh=parent+'/body.gltf', dump=str(dump), sourceSha256=sha(raw)))
        print(f'Converted {number}/{len(active)}: {parent}', flush=True)
    write(pack/'manifest.json', manifest)
    write(pack/'provenance.json', dict(format='gt4-source-conversion', models=provenance,
                                     profilesSha256=sha(ROOT/'scripts/gt4_profiles.json')))
    return manifest


def extract_gt2(tool, disc, directory, catalog, logs):
    catalog.mkdir(parents=True)
    command([tool, 'export-cars', disc, catalog, '--no-mesh'], logs/'gt2-catalog.log')
    listing = command([tool, 'ls', disc], logs/'gt2-volume.log', capture=True)
    paths = [row.split(None, 1)[1].strip() for row in listing.splitlines() if len(row.split(None, 1)) == 2]
    # Original GT2 names can contain legacy single-byte punctuation. Only the
    # ASCII car ID is needed here; the expansion builder applies the same decode.
    donors = [json.loads(p.read_text(encoding='utf-8-sig', errors='replace'))['carId'] for p in catalog.glob('*.json')]
    wanted = {f'carobj/{car}.{ext}' for car in donors for ext in ('cdo', 'cdp')}
    chosen = []
    for path in paths:
        plain = path.removesuffix('.gz')
        if (plain in wanted or path.startswith('.carinfo') or path in ('.carcolor', '.cclatain') or
            plain in [f'carparam/{locale}_{kind}.dat' for locale in ('usa','eng') for kind in ('unistrdb','gtmode_data')]):
            chosen.append(path)
    for number, path in enumerate(chosen):
        output = directory/path
        output.parent.mkdir(parents=True, exist_ok=True)
        command([tool, 'extract', disc, path, output], logs/f'gt2-extract-{number}.log')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--simulation', type=Path, required=True, help='Owned GT2 USA Simulation disc or prepared directory')
    parser.add_argument('--arcade', type=Path, help='Optional owned GT2 USA Arcade disc or prepared directory')
    parser.add_argument('--output', type=Path, required=True, help='New directory under this checkout work/')
    parser.add_argument('--build', '--tools', type=Path, default=ROOT/'build')
    parser.add_argument('--model-tool', type=Path, help='Source-built pinned GTPS2ModelTool DLL or executable')
    parser.add_argument('--tool-cache', type=Path, default=ROOT/'work/gt4-tools')
    parser.add_argument('--catalog-tool', type=Path, help='Source-built gt4catalog.dll; otherwise build the bundled project')
    parser.add_argument('--pdtools-root', type=Path, help='Pinned PDTools source checkout when building gt4catalog')
    parser.add_argument('--offline', action='store_true', help='Require prebuilt model and catalogue tools; never fetch sources')
    parser.add_argument('--models', nargs='+', help='Diagnostic subset only; does not create activation-ready overlays')
    parser.add_argument('--workers', type=int, default=4)
    args = parser.parse_args()
    for field in ('iso','simulation','arcade','output','build','model_tool','tool_cache','catalog_tool','pdtools_root'):
        value = getattr(args, field)
        if value is not None:
            setattr(args, field, value.resolve())
    if args.output.exists() or args.output.with_name(args.output.name+'.preparing').exists():
        parser.error('Output or its .preparing directory already exists; choose a new directory')
    if not args.output.is_relative_to((ROOT/'work').resolve()):
        parser.error('Generated owned data must be written under this checkout work/')
    if args.workers < 1 or args.workers > 16:
        parser.error('Workers must be between 1 and 16')
    names = ('gt2tool','gt4assettool','gt4nativemetadata','gt4materialmetadata','gt4specdbexport','gt4showroomexport')
    tools = {name: native_tool(args.build, name) for name in names}
    model_tool = args.model_tool
    if model_tool is None:
        if args.offline:
            model_tool = args.tool_cache/'bin/GTPS2ModelTool.dll'
        else:
            model_tool = build_tool(args.tool_cache)
    if not model_tool.is_file():
        parser.error('Source-built model tool not found; pass --model-tool')
    if args.offline and (args.catalog_tool is None or not args.catalog_tool.is_file()):
        parser.error('--offline requires --catalog-tool pointing to a prebuilt gt4catalog.dll')
    stage = args.output.with_name(args.output.name+'.preparing')
    stage.mkdir(parents=True)
    # Prepared discs can already have an expansion. Export only their original
    # tables, so a second preparation never appends the expansion to itself.
    os.environ['GT2_CAR_EXPANSION'] = str(stage/'disabled-expansion')
    logs = stage/'logs'
    catalog_tool = args.catalog_tool
    if catalog_tool is None:
        pdtools = args.pdtools_root
        if pdtools is None:
            candidates = list((args.tool_cache/('GTPS2ModelTool-'+TOOL_COMMIT)).glob('*/PDTools'))
            if len(candidates) == 1:
                pdtools = candidates[0]
        if pdtools is None or not (pdtools/'PDTools.SpecDB/PDTools.SpecDB.csproj').is_file():
            raise ValueError('Pass --pdtools-root with the pinned PDTools source tree')
        target = stage/'tools/catalog'
        command(['dotnet','build',ROOT/'tools/gt4catalog/gt4catalog.csproj','-c','Release','-o',target,
                 '-p:PDToolsRoot='+str(pdtools),'-p:NuGetAudit=false'], logs/'build-catalog.log')
        catalog_tool = target/'gt4catalog.dll'
    profiles = read(ROOT/'scripts/gt4_profiles.json')
    source = extract_gt4(args.iso, stage/'vol', profiles)
    specdb = stage/'vol/specdb/GT4_US2560'
    command([*model_command(catalog_tool),specdb,stage/'catalog.json','american','--physics'], logs/'catalog.log')
    added_catalog = read(stage/'catalog.json')
    if len(added_catalog) != 722:
        raise ValueError('Expected 722 named GT4 catalogue records')
    for mode, disc in [('simulation',args.simulation),('arcade',args.arcade)]:
        if disc is None:
            continue
        extract_gt2(tools['gt2tool'],disc,stage/'gt2-assets'/mode,stage/'gt2-catalog'/mode,logs/mode)
        script('build_gt4_expansion.py', ['--assets',stage/'gt2-assets'/mode,'--catalog',stage/'catalog.json',
            '--physics',stage/'catalog.physics.json','--gt2-catalog',stage/'gt2-catalog'/mode,
            '--maker-names',ROOT/'db/gt4-maker-map.json','--output',stage/'expansions'/mode],logs/f'expansion-{mode}.log')
    added = read(stage/'expansions/simulation/assets/carparam/gt4_expansion.json')['cars']
    pack = stage/'modelsets/gt4'
    manifest = build_models(pack,stage/'vol',profiles,added,read(ROOT/'db/gt4-expansion-models.json')['cars'],model_tool,args.models)
    script('build_gt4_materials.py',['--pack',pack,'--output',stage/'alignment','--tool',tools['gt4materialmetadata'],
                                  '--workers',args.workers],logs/'alignment.log')
    alignment = read(stage/'alignment/report.json')
    for row in alignment:
        row['source'] = row['native']
        row['rawSha256'] = sha(Path(row['source']))
    write(stage/'alignment.json',alignment)
    script('build_gt4_paints.py',['--pack',pack,'--output',stage/'paints','--vol',stage/'vol',
                                '--tool',tools['gt4assettool'],'--workers',args.workers],logs/'paints.log')
    script('build_gt4_native.py',['--pack',pack,'--paints',stage/'paints','--output',stage/'native',
        '--tool',tools['gt4nativemetadata'],'--alignment',stage/'alignment.json','--vol',stage/'vol',
        '--workers',args.workers],logs/'native.log')
    command([tools['gt4specdbexport'],specdb,stage/'specdb-json'],logs/'specdb-json.log')
    script('build_gt4_showroom.py',['--tool',tools['gt4showroomexport'],'--core',stage/'vol/CORE.elf',
        '--specdb',specdb,'--specdb-json',stage/'specdb-json','--alignment',stage/'native/report.json',
        '--output',stage/'showroom.json'],logs/'showroom.log')
    script('build_gt4_wheels.py',['--specdb',stage/'specdb-json','--alignment',stage/'native/report.json',
        '--showroom',stage/'showroom.json','--output',stage/'native/native-v16/wheels.json'],logs/'wheels.log')
    shutil.copytree(stage/'native/native-v16',pack/'native-v16')
    interiors = read(pack/'native-v16/interiors.json')['cars']
    authored = sum(bool({**entry,**interiors.get(car,{})}.get('nativeInterior')) for car,entry in manifest['cars'].items())
    if not args.models and (len(manifest['cars']) != 1110 or authored != 89):
        raise ValueError('Final GT4 model/interior catalogue count differs')
    # A diagnostic subset must never activate a catalogue whose required meshes
    # are absent. Keep its data for inspection, but remove activation markers.
    if args.models:
        for marker in (stage/'expansions').glob('*/catalogue.version'):
            marker.unlink()
    ready = dict(version='0.9.0',source=source,complete=not bool(args.models),
        modelMappings=len(manifest['cars']),authoredInteriorMappings=authored,
        expansionCars=722 if not args.models else 0,discs=['simulation']+(['arcade'] if args.arcade else []))
    stage.rename(args.output)
    finalize_evidence(args.output, stage)
    write(args.output/'READY.json', ready)
    print('GT4 expansion prepared:',args.output,flush=True)


if __name__ == '__main__':
    try:
        main()
    except (ValueError,OSError,RuntimeError,subprocess.SubprocessError) as error:
        print('Preparation failed:',error,file=sys.stderr)
        sys.exit(1)
