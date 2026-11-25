"""Validate and attach the reviewed R32 cockpit family to a GT4 model pack."""
import hashlib
import json
from pathlib import Path
import shutil
import struct

ROOT = Path(__file__).resolve().parents[1] / 'assets/cockpits/r32'
CARS = {'nn32n','nr02n','nr32n','nv12n','nv22n','z0351','z0773','z0906','z0907','z0908','z0909','z0910'}

def validate_glb(data):
    if len(data)<20 or data[:4]!=b'glTF' or struct.unpack_from('<II',data,4)!=(2,len(data)):
        raise ValueError('Invalid cockpit GLB')
    size,kind=struct.unpack_from('<II',data,12)
    if kind!=0x4e4f534a or 20+size>len(data):raise ValueError('Invalid cockpit GLB document')
    doc=json.loads(data[20:20+size])
    if any('uri' in item for key in ('buffers','images') for item in doc.get(key,[])):
        raise ValueError('Cockpit GLB must be self-contained')

def assets(root=ROOT):
    root=Path(root);inventory=json.loads((root/'inventory.json').read_text())
    if inventory.get('format')!='gt2-r32-cockpits' or inventory.get('version')!=1 or set(inventory['cars'])!=CARS:
        raise ValueError('Expected all twelve R32 cockpit mappings')
    result={}
    for item in inventory['files']:
        name=item['path'];path=(root/name).resolve()
        if not path.is_relative_to(root.resolve()) or name in result:raise ValueError('Unsafe cockpit inventory')
        data=path.read_bytes()
        if hashlib.sha256(data).hexdigest()!=item['sha256']:raise ValueError('Cockpit checksum mismatch: '+name)
        result[name]=path
    expected=set()
    for car in sorted(CARS):
        manifest=json.loads(result[car+'/manifest.json'].read_text())
        if manifest.get('format')!='gt2-authored-car' or manifest.get('version')!=1 or manifest.get('car')!=car or manifest.get('mesh')!='r32.glb':
            raise ValueError('Invalid cockpit manifest: '+car)
        expected.update(car+'/'+name for name in ('manifest.json','attribution.json','r32.glb'))
        validate_glb(result[car+'/r32.glb'].read_bytes())
        attribution=json.loads(result[car+'/attribution.json'].read_text())
        if not all(attribution.get(k) for k in ('author','source','license','changes')):raise ValueError('Missing cockpit attribution')
    if set(result)!=expected:raise ValueError('Unexpected cockpit payload')
    return result

def attach(pack):
    pack=Path(pack);catalogue=json.loads((pack/'manifest.json').read_text())['cars']
    for name,source in assets().items():
        if name.split('/')[0] not in catalogue:continue
        target=pack/'authored'/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)
