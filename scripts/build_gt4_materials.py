"""Add original GT4 alpha state and callback-selected lamps without replacing body geometry.

Development inputs: owned CAR4 files and the gt4formats metadata tool.
All generated data must stay in a fresh directory under work.
"""
import argparse
import concurrent.futures
import hashlib
import json
import re
import struct
import statistics
import subprocess
from pathlib import Path
from PIL import Image

def write(path,value):
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(value,separators=(',',':'))+'\n',encoding='utf8')

def draw_key(d):
    return d['shape'],d['packet'],d['material'],d['texset'],d['texture']

def build(job,args):
    parent,entry,ids,dump=job
    raw=dump.with_name(dump.name.removesuffix('_dump'))
    if not raw.is_file():raise ValueError('Missing original CAR4: '+str(raw))
    native=args.output/'native'/parent
    if not (native/'draws.json').is_file():
        subprocess.run([str(args.tool),str(raw),str(native)],check=True,capture_output=True)
    states=json.loads((native/'draws.json').read_text())['states']
    geometry={}
    for state in states:
        for d in state:
            if d['vertices']:geometry[draw_key(d)]=d['vertices']
            else:d['vertices']=geometry[draw_key(d)]
    stats=json.loads((args.pack/parent/'conversion.json').read_text())
    result={'format':'gt4-materials','version':1,'meshes':{}}
    glass=0
    # The existing export and the native parser use different packed-position
    # units. Derive their transform from matching original body draws, not a
    # hand-picked lamp offset or a depth bias (which could expose hidden faces).
    samples=[];centers=[]
    for original in stats['exterior']['shapes']:
        sm=re.match(r'shape(\d+)',original['shape']);tm=re.search(r'Tex(-?\d+)',original['material'])
        if not sm or not tm:continue
        candidates=[d for d in states[0] if d['shape']==int(sm[1]) and d['texture']==int(tm[1])]
        vertices=[v for d in candidates for v in d['vertices']]
        if not vertices:continue
        lo=[min(v[k] for v in vertices) for k in range(3)];hi=[max(v[k] for v in vertices) for k in range(3)]
        old_lo=original['min'];old_hi=original['max']
        for k in range(3):
            extent=hi[k]-lo[k];old_extent=old_hi[k]-old_lo[k]
            if extent>.1 and old_extent>.1:samples.append(old_extent/extent)
        centers.append(([.5*(lo[k]+hi[k]) for k in range(3)],[.5*(old_lo[k]+old_hi[k]) for k in range(3)]))
    if not samples:raise ValueError('No matching body geometry for lamp alignment: '+parent)
    native_scale=statistics.median(samples)
    if not .01<native_scale<100:raise ValueError('Invalid native position unit conversion: '+parent)
    inliers=sum(abs(r/native_scale-1)<.002 for r in samples)
    if inliers<len(samples)*.6:raise ValueError('Native body coordinates do not agree on scale: '+parent)
    native_offset=[statistics.median(old[k]-native_scale*point[k] for point,old in centers) for k in range(3)]
    for name,part in [('mesh','exterior'),('cockpitMesh','cockpit')]:
        path=args.pack/entry[name];doc=json.loads(path.read_text())
        prims=doc['meshes'][0]['primitives'];shapes=stats[part]['shapes']
        if len(prims)!=len(shapes):raise ValueError('Conversion primitive count mismatch: '+str(path))
        changes=[]
        for i,(p,s) in enumerate(zip(prims,shapes)):
            mat=doc['materials'][p['material']]
            match=re.match(r'shape(\d+)',s['shape']);shape=int(match[1])
            texture=-1
            if 'baseColorTexture' in mat['pbrMetallicRoughness']:
                ti=mat['pbrMetallicRoughness']['baseColorTexture']['index']
                uri=doc['images'][doc['textures'][ti]['source']]['uri']
                tm=re.search(r'(\d+)\.(\d+)\.png$',uri)
                if tm:texture=int(tm[2])
            candidates=[d for d in states[0] if d['shape']==shape and d['texture']==texture]
            if not candidates:continue
            # A shared texture can be paint alpha / a mask in one draw and
            # transparent glass in another. Only original PRIM.ABE + ALPHA 44
            # restores coverage blending, never the texture's darkness/name.
            kinds={(d['blend'],d['blendEquation']) for d in candidates}
            if len(kinds)!=1:raise ValueError('Ambiguous GS blend state: '+str(path)+' '+s['shape'])
            blend,equation=next(iter(kinds))
            if blend and equation==0x44:
                changes.append({'index':i,'alphaBlend':True})
                glass+=1
        result['meshes'][name]={'sha1':hashlib.sha1(path.read_bytes()).hexdigest(),'primitives':changes}

    # Restore only additional callback geometry. Shared body shapes keep their
    # existing geometry/material; lamp primitives use the actual PS2 UVs/colours.
    off={draw_key(d) for d in states[0]}
    existing_geometry={(d['shape'],d['packet'],d['texset'],d['texture']) for d in states[0]}
    extra={}
    for state_index,state in enumerate(states):
        for d in state:
            k=draw_key(d)
            if k in off or (d['shape'],d['packet'],d['texset'],d['texture']) in existing_geometry:continue
            if k not in extra:extra[k]=[d,0]
            extra[k][1]|=1<<state_index
    dest=args.output/'materials'/parent
    gltf={'asset':{'version':'2.0','generator':'GT2 native GT4 lamp state importer'},
          'scene':0,'scenes':[{'nodes':[0]}],'nodes':[{'mesh':0}],
          'meshes':[{'primitives':[]}],'materials':[],'images':[],'textures':[],
          'samplers':[{'wrapS':33071,'wrapT':33071}], 'buffers':[],'bufferViews':[],'accessors':[]}
    binary=bytearray();textures={}
    def acc(values,width):
        at=len(binary)
        for v in values:binary.extend(struct.pack('<'+'f'*width,*v))
        view=len(gltf['bufferViews']);gltf['bufferViews'].append({'buffer':0,'byteOffset':at,'byteLength':len(binary)-at})
        i=len(gltf['accessors']);gltf['accessors'].append({'bufferView':view,'componentType':5126,'count':len(values),'type':'VEC'+str(width)})
        return i
    for d,mask in extra.values():
        positions=[[v[k]*native_scale+native_offset[k] for k in range(3)] for v in d['vertices']]
        if not positions:continue
        pbr={'baseColorFactor':[1,1,1,1],'metallicFactor':0,'roughnessFactor':1}
        if d['texture']>=0:
            key=d['texset'],d['texture']
            if key not in textures:
                data=(native/(str(key[0])+'.'+str(key[1])+'.rgba')).read_bytes();w,h=struct.unpack('<II',data[:8])
                rgba=bytearray(data[8:]);assert len(rgba)==w*h*4
                for i in range(3,len(rgba),4):rgba[i]=min(255,rgba[i]*2)
                image=Image.frombytes('RGBA',(w,h),bytes(rgba));fn=f'{key[0]}.{key[1]}.png'
                dest.mkdir(parents=True,exist_ok=True);image.save(dest/fn)
                textures[key]=len(gltf['images']);gltf['images'].append({'uri':fn})
                gltf['textures'].append({'source':textures[key],'sampler':0})
            pbr['baseColorTexture']={'index':textures[key]}
        material=len(gltf['materials'])
        blend=bool(d['blend'])
        gltf['materials'].append({'name':f"native-lamp-{d['shape']}-{d['packet']}",'alphaMode':'BLEND' if blend else 'MASK',
            'doubleSided':True,'pbrMetallicRoughness':pbr,
            'extras':{'gt2pc':{'lampStates':mask,'emissive':True}}})
        colors=[]
        for v in d['vertices']:
            colors.append([min(1,max(0,v[8+k]*(d['colors'][4+k]+d['colors'][12+k]))) for k in range(3)])
        gltf['meshes'][0]['primitives'].append({'attributes':{'POSITION':acc(positions,3),
            'NORMAL':acc([v[3:6] for v in d['vertices']],3),'TEXCOORD_0':acc([v[6:8] for v in d['vertices']],2),
            'COLOR_0':acc(colors,3)},'material':material,'mode':4})
    if binary:
        gltf['buffers']=[{'uri':'lamps.bin','byteLength':len(binary)}]
        (dest/'lamps.bin').write_bytes(binary);write(dest/'lamps.gltf',gltf)
        result['lampMesh']=parent+'/lamps.gltf'
    write(dest/'materials.json',result)
    return {'parent':parent,'ids':ids,'glassPrimitives':glass,'lampPrimitives':len(gltf['meshes'][0]['primitives']),
            'lampVertices':sum(len(d['vertices']) for d,mask in extra.values()),'native':str(raw),'nativeScale':native_scale,'nativeOffset':native_offset,'scaleInliers':inliers,'scaleSamples':len(samples)}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for key in ('pack','output','tool'):p.add_argument('--'+key,type=Path,required=True)
    p.add_argument('--cars',nargs='+');p.add_argument('--workers',type=int,default=4);a=p.parse_args()
    a.pack=a.pack.resolve();a.output=a.output.resolve();a.tool=a.tool.resolve()
    if not a.output.is_relative_to(Path('work').resolve()):p.error('Output must be under work')
    doc=json.loads((a.pack/'manifest.json').read_text());prov=json.loads((a.pack/'provenance.json').read_text())
    dumps={Path(m['mesh']).parent.as_posix():Path(m['dump']) for m in prov['models']}
    jobs={}
    for id,e in doc['cars'].items():
        if a.cars and id not in a.cars:continue
        parent=Path(e['mesh']).parent.as_posix();jobs.setdefault(parent,[parent,e,[],dumps[parent]])[2].append(id)
    a.output.mkdir(parents=True,exist_ok=True);report=[];index={'format':'gt4-material-overlay','version':1,'cars':{}}
    with concurrent.futures.ThreadPoolExecutor(a.workers) as pool:
        for row in pool.map(lambda j:build(j,a),jobs.values()):
            report.append(row)
            for id in row['ids']:index['cars'][id]=row['parent']+'/materials.json'
    write(a.output/'materials'/'manifest.json',index);write(a.output/'report.json',report)
    print(json.dumps({'models':len(report),'cars':len(index['cars']),
        'glassPrimitives':sum(r['glassPrimitives'] for r in report),'lampPrimitives':sum(r['lampPrimitives'] for r in report)}))
if __name__=='__main__':main()
