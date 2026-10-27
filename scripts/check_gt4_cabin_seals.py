"""Mandatory closed-panel colour gate for a GT4 candidate, using owned assets.

A successful process alone is insufficient: every manifest ID must have all
48 eye views and zero witness leaks. The report binds results to source/binary
and catalogue hashes, so a previous candidate's pass cannot authorise a new one.
"""
import argparse
import csv
import gzip
import hashlib
import json
import os
from pathlib import Path
import subprocess
from concurrent.futures import ThreadPoolExecutor

SOURCES = [
    'src/gt2view/scene_assets.h', 'src/gt2view/procedural_cockpit.cpp',
    'src/gt2view/model_texture_atlas.h', 'src/gt2view/decoded_textures.cpp',
    'src/gt2view/vk_scene_renderer.h',
    'src/gt2view/procedural_cockpit.h', 'src/gt2view/cockpit_fit.h',
    'src/gt2view/vk_scene_renderer.cpp', 'src/gt2view/scene_types.h',
    'src/gt2view/shaders/scene.vert', 'src/gt2view/shaders/scene_stereo.vert',
    'src/gt2view/shaders/scene.frag', 'src/gt2view/shaders/scene_cached.frag',
    'src/gt2view/shaders/native_model_vertex.glsl',
    'src/gt2view/shaders/native_model_fragment.glsl',
    'src/gt2view/shaders/scene_model_common.glsl',
    'src/gt2view/shaders/paint_clearcoat.glsl',
    'tests/gt4_cabin_seal_audit.cpp', 'tests/gt4_cabin_witness.h',
    'src/gt2view/gt4_cabin_geometry.h', 'tools/gt2game/cockpit_camera.h',
    'src/gt2view/model_glazing.h', 'src/gt2view/model_window_cut.h',
    'tests/gt4_material_contract_audit.cpp',
    'src/gt2formats/model_set.h', 'src/gt2formats/gltf_reader.h',
    'src/gt2formats/json.h', 'src/gt2formats/json.cpp',
    'src/gt2formats/gltf_reader.cpp', 'tests/offscreen_car_audit.h',
    'scripts/build_gt4_native.py', 'scripts/build_gt4_wheels.py',
    'scripts/gt4_native_metadata.cpp', 'scripts/gt4_attachment_trace.h',
]

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def asset_fingerprint(native):
    # Follow explicit catalogue/glTF/palette references. Never recurse through
    # the pack's junctions: the same source textures have several lexical roots.
    def document(path):
        data=path.read_bytes()
        return json.loads(gzip.decompress(data) if path.suffix=='.gz' else data)
    def relative(root,name,source_uri=False):
        path=Path(name)
        if path.is_absolute() or ':' in name or (not source_uri and '..' in path.parts):
            raise RuntimeError(f'Unsafe model asset {name}')
        # Authored texture URIs can address the neighbouring base-model pack.
        # Keep lexical roots; resolve() would change junction-relative URIs.
        candidate=Path(os.path.abspath(root/path))
        candidate.relative_to(Path(os.path.abspath(native.parent if source_uri else native)))
        return candidate
    assets=set()
    if (native/'wheels.json').is_file(): assets.add(native/'wheels.json')
    catalogue=document(native/'manifest.json')
    for name in set(catalogue['cars'].values()):
        mesh=relative(native,name);assets.add(mesh)
        gltf=document(mesh)
        for kind in ('buffers','images'):
            for record in gltf.get(kind,[]):
                assets.add(relative(mesh.parent,record['uri'],True))
        palette=mesh.parent/'paints.json.gz';assets.add(palette)
        for colour in document(palette)['colors']:
            for image in colour['images'].values():
                assets.add(relative(native,image))
    aggregate=hashlib.sha256()
    for path in sorted(assets,key=lambda p:p.as_posix()):
        name=Path(os.path.relpath(path,native)).as_posix()
        aggregate.update(name.encode('utf-8')+b'\0'+bytes.fromhex(sha(path)))
    return {'files':len(assets),'sha256':aggregate.hexdigest()}

def provenance(args):
    files = {name: sha(name) for name in SOURCES}
    files['catalogue'] = sha(args.pack / 'manifest.json')
    files['native_catalogue'] = sha(args.native / 'manifest.json')
    files['audit_binary'] = sha(args.exe)
    files['native_assets'] = asset_fingerprint(args.native)
    files['disc'] = sha(args.disc)
    return files

def validate(csv_path, expected):
    seen = {car: set() for car in expected}
    holes = []
    with csv_path.open(newline='') as file:
        for row in csv.DictReader(file):
            car = row['car']
            if car not in seen:
                raise RuntimeError(f'Unexpected car {car}')
            key = tuple(int(row[k]) for k in ('pose', 'offset', 'eye'))
            if key in seen[car]:
                raise RuntimeError(f'Duplicate view {car} {key}')
            seen[car].add(key)
            if int(row['yellow_pixels']):
                holes.append((car, key, int(row['yellow_pixels'])))
    required = {(p, s, e) for p in range(8) for s in range(3) for e in range(2)}
    missing = [car for car, views in seen.items() if views != required]
    if missing or holes:
        raise RuntimeError(f'Incomplete cars: {missing[:12]}; leaking views: {len(holes)}, examples: {holes[:12]}')
    return len(expected) * len(required)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--disc', required=True, type=Path)
    parser.add_argument('--pack', required=True, type=Path)
    parser.add_argument('--native', required=True, type=Path)
    parser.add_argument('--expansion', required=True, type=Path)
    parser.add_argument('--exe', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--verify-report', action='store_true')
    parser.add_argument('--workers', type=int, default=1, choices=range(1,5))
    args = parser.parse_args()
    # Preserve the scoped lexical path when captures are a junction to H:.
    args.output = Path(os.path.abspath(args.output))
    args.output.relative_to(Path('work').absolute())
    args.output.mkdir(parents=True, exist_ok=True)
    expected = set(json.loads((args.pack/'manifest.json').read_text(encoding='utf-8'))['cars'])
    if not expected:
        raise RuntimeError('Empty catalogue')
    before = provenance(args)
    if args.verify_report:
        report = json.loads((args.output/'gate.json').read_text(encoding='utf-8'))
        if (report.get('format') != 'gt4-cabin-seal-gate' or not report.get('passed')
                or report.get('inputs') != before
                or report.get('csv_sha256') != sha(args.output/'seals.csv')):
            raise RuntimeError('Missing, stale or modified colour gate')
        views = validate(args.output/'seals.csv', expected)
        if report.get('cars') != len(expected) or report.get('eye_views') != views or report.get('leaking_views') != 0:
            raise RuntimeError('Invalid gate coverage')
        print(f'Current colour gate verified: {len(expected)} cars, {views} eye views')
        return
    if not args.verify_report:
        env = os.environ.copy()
        env.update(GT2_CAR_EXPANSION=str(args.expansion.resolve()),
                   GT2_GT4_NATIVE=str(args.native.absolute()), GT2_DEBUG_CABIN='1')
        ids=sorted(expected)
        def render(index):
            dest=args.output/f'shard-{index}';dest.mkdir(exist_ok=True)
            with (dest/'render.log').open('w') as log:
                result=subprocess.run([str(args.exe.resolve()), str(args.disc.resolve()),
                    str(args.pack.absolute()), str(dest), *ids[index::args.workers]], env=env,
                    stdout=log, stderr=subprocess.STDOUT, creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            return result.returncode
        with ThreadPoolExecutor(max_workers=args.workers) as pool:
            codes=list(pool.map(render,range(args.workers)))
        if any(codes):
            raise RuntimeError(f'Colour audit failed: {codes}; inspect {args.output}/shard-*/render.log')
        with (args.output/'seals.csv').open('w',newline='') as target:
            writer=None
            for index in range(args.workers):
                with (args.output/f'shard-{index}'/'seals.csv').open(newline='') as source:
                    reader=csv.DictReader(source)
                    if writer is None:writer=csv.DictWriter(target,fieldnames=reader.fieldnames);writer.writeheader()
                    elif reader.fieldnames!=writer.fieldnames:raise RuntimeError('Mismatched shard columns')
                    writer.writerows(reader)
    views = validate(args.output/'seals.csv', expected)
    if provenance(args) != before:
        raise RuntimeError('Inputs changed during audit')
    report = {'format': 'gt4-cabin-seal-gate', 'version': 1, 'passed': True,
              'cars': len(expected), 'eye_views': views, 'msaa': 2,
              'leaking_views': 0, 'inputs': before, 'csv_sha256': sha(args.output/'seals.csv')}
    (args.output/'gate.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(f'PASS: {len(expected)} cars, {views} eye views, zero closed-panel leaks')

if __name__ == '__main__':
    main()
