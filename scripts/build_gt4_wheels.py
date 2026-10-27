"""Export recovered GT4 showroom wheel/body pose for the native model overlay.

CORE.GT4 US 1.01: 004413c8, 00441b20, 00442800, 00361908, 0034ba30.
CORE.GT4 SHA1: 30031137ce494323816bb707b90d3ec0d07937d2.
Field derivation and Ford Ka oracle check: gt4-pc/docs/formats/specdb.md.
00459780 / 00459c10 use work+914-work+918, not the body's lowest vertex.
Inputs include a hash-bound native showroom export and importer alignment.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct


def tyre(b):
    aspect, _, rim, width5 = b
    width = (width5 * 5 or 10) * .001
    diameter = rim if rim < 61 else rim / 10
    return width, diameter * .0254 / 2 + width * aspect * .05


def geometry(rows, model):
    variations = {i for i, label in rows['labels'].items() if label.rsplit('_', 1)[0] == model}
    gid = next(i for i, b in rows['CAR_VARIATION_american'].items()
               if struct.unpack_from('<I', b)[0] in variations)
    dp = struct.unpack_from('<I', rows['GENERIC_CAR'][gid])[0]
    parts = {table: index for index, table in struct.iter_unpack('<iI', rows['DEFAULT_PARTS'][dp]) if index >= 0}
    chassis = rows['CHASSIS'][parts[6]]
    modify = rows['RACINGMODIFY'][parts[7]]
    wheelbase = struct.unpack_from('<h', chassis, 4)[0] * .001
    track = struct.unpack_from('<hh', modify, 2)
    reduction = struct.unpack_from('<bb', modify, 0x13)
    wells = []
    for axle, table, name in [(0, 25, 'FRONTTIRE'), (1, 26, 'REARTIRE')]:
        size = struct.unpack_from('<i', rows[name][parts[table]])[0]
        width, radius = tyre(rows['TIRESIZE'][size])
        half_track = (track[axle] - 2 * reduction[axle]) * .0005
        for sign in [-1, 1]:
            wells.append([sign * half_track, radius, (-1 if axle == 0 else 1) * wheelbase / 2, radius, width])
    return wells


def finite_vector(value, size, name):
    if len(value) != size or any(not isinstance(x, (int, float)) or not math.isfinite(x) for x in value):
        raise ValueError('Invalid showroom '+name)
    return value


def validate_rotation(rotation, name):
    for c in range(3):
        for d in range(3):
            if abs(sum(rotation[3*c+r]*rotation[3*d+r] for r in range(3))-(c==d))>2e-5:
                raise ValueError('Invalid showroom '+name)
    determinant=(rotation[0]*(rotation[4]*rotation[8]-rotation[7]*rotation[5])
                 -rotation[3]*(rotation[1]*rotation[8]-rotation[7]*rotation[2])
                 +rotation[6]*(rotation[1]*rotation[5]-rotation[4]*rotation[2]))
    if determinant<.99998: raise ValueError('Invalid showroom reflected '+name)


def aligned_pose(record, native):
    """Preserve body-local wheel anchors and the separate static body rotation.

    Source is +X right, +Y up, -Z front. Asset alignment is p*s+offset.
    Rotate aligned geometry about bodyPivot. ground=-bodyHeight+offsetY
    makes the existing runtime ground anchor supply the static translation;
    do not add bodyHeight a second time. The ground anchor cannot replace
    the independently retained body rotation. wheelBasis includes that
    rotation already and acts on donor wheel vectors, not anchor positions.
    """
    scale = record['nativeScale']; offset = finite_vector(record['nativeOffset'], 3, 'alignment')
    if not math.isfinite(scale) or scale <= 0: raise ValueError('Invalid native scale')
    height, pitch = finite_vector(native['body'], 2, 'body')
    matrix = finite_vector(native['bodyMatrix'], 16, 'body matrix')
    if abs(matrix[12])+abs(matrix[14])+abs(matrix[15]-1) > 1e-5 or abs(matrix[13]-height)>1e-5:
        raise ValueError('Invalid showroom body translation')
    rotation = [matrix[4*c+r] for c in range(3) for r in range(3)]
    validate_rotation(rotation,'body rotation')
    if len(native['sourceWheel'])!=4 or any(type(i) is not int or not 0<=i<4 for i in native['sourceWheel']):
        raise ValueError('Invalid showroom source wheel mapping')
    if len(native['wheels']) != 4: raise ValueError('Invalid showroom wheel count')
    wheels=[]; angles=[]; bases=[]
    for wheel in native['wheels']:
        xyz=finite_vector(wheel['position'],3,'position')
        width,radius,_=finite_vector(wheel['dimensions'],3,'dimensions')
        if width<.005 or radius<.05: raise ValueError('Invalid showroom wheel size')
        wheels.append([*(xyz[k]*scale+offset[k] for k in range(3)),radius*scale,width*scale])
        angles.append(finite_vector(wheel['angles'],3,'angles'))
        basis=finite_vector(wheel['donorBasis'],9,'donor wheel basis')
        validate_rotation(basis,'donor wheel basis')
        bases.append(basis)
    return dict(ground=-height*scale+offset[1],wheels=wheels,
                bodyHeight=height*scale,bodyPitchDegrees=pitch,bodyRotation=rotation,
                bodyPivot=offset,wheelAngles=angles,wheelBasis=bases,sourceWheel=native['sourceWheel'])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ['specdb', 'alignment', 'output']: p.add_argument('--'+name, required=True, type=Path)
    p.add_argument('--showroom', required=True, type=Path)
    p.add_argument('--audit', type=Path)
    a = p.parse_args()
    rows = {}; hashes = {}
    for name in ['CAR_VARIATION_american', 'GENERIC_CAR', 'DEFAULT_PARTS', 'CHASSIS',
                 'RACINGMODIFY', 'FRONTTIRE', 'REARTIRE', 'TIRESIZE', 'VARIATIONamerican']:
        data = (a.specdb / (name + '.json')).read_bytes()
        hashes[name] = hashlib.sha256(data).hexdigest()
        records = json.loads(data)['rows']
        if name == 'VARIATIONamerican': rows['labels'] = {r['id']: r['label'] for r in records}
        else: rows[name] = {r['id']: bytes.fromhex(r['hex']) for r in records}
    native=json.loads(a.showroom.read_text(encoding='utf-8-sig'))
    if native.get('format')!='gt4-showroom-geometry' or native.get('version')!=1:
        raise ValueError('Unsupported native showroom geometry')
    if native.get('specdbSources')!=hashes: raise ValueError('Showroom/SpecDB source hash mismatch')
    declared_missing=set(native['missingModels']);cars={};missing={};pitch_cases={}
    records=json.loads(a.alignment.read_text())
    for record in records:
        source=Path(record['source']);code=source.name
        if code not in native['models']:
            if code not in declared_missing: raise ValueError('Unreviewed missing showroom model: '+code)
            missing.setdefault(code,[]).extend(record['ids']);continue
        row=aligned_pose(record,native['models'][code])
        digest=hashlib.sha256(source.read_bytes()).hexdigest()
        if digest!=record['rawSha256']: raise ValueError('Alignment/source hash mismatch: '+code)
        row.update(model=code,sourceSha256=digest)
        for id in record['ids']: cars[id] = row
        if abs(row['bodyPitchDegrees'])>1e-5:pitch_cases[code]=dict(degrees=row['bodyPitchDegrees'],ids=record['ids'])
    audit=dict(alignmentModels=len(records),catalogueMappings=sum(len(r['ids']) for r in records),
               exportedMappings=len(cars),missingMappings=missing,pitchModels=pitch_cases,
               showroomSha256=hashlib.sha256(a.showroom.read_bytes()).hexdigest(),
               alignmentSha256=hashlib.sha256(a.alignment.read_bytes()).hexdigest())
    result = dict(format='gt4-wheel-geometry', version=2, sources=hashes, cars=cars,
                  missing=sorted(missing),poseSource=native['provenance'],audit=audit)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(json.dumps(result, separators=(',', ':'))+'\n')
    if a.audit:a.audit.write_text(json.dumps(audit,indent=2)+'\n')
    print(f'Wheel geometry: {len(cars)} cars; missing models: {sorted(missing)}; pitch models: {len(pitch_cases)}')


if __name__ == '__main__': main()
