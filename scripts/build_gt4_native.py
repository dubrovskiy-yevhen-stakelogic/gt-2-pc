"""Preserve the owned GT4 car's complete colour draw program, not OBJ heuristics.

Generated assets and source extracts stay under work. The baseline pack and
the GT4 parser checkout are read-only inputs.
"""
import argparse, concurrent.futures, gzip, hashlib, json, re, struct, subprocess
from pathlib import Path
from PIL import Image
import numpy as np
from gt4_native_instances import resolve_draws
from gt4_native_cabins import merge_cabin,split_exterior_glazing

def write_bytes(p,data):
    p.parent.mkdir(parents=True,exist_ok=True)
    if p.is_file() and p.read_bytes()==data:return
    temporary=p.with_name(p.name+'.tmp');temporary.write_bytes(data);temporary.replace(p)

def write(p,v):
    data=(json.dumps(v,separators=(',',':'))+'\n').encode('utf8')
    write_bytes(p,gzip.compress(data,compresslevel=6,mtime=0) if p.suffix=='.gz' else data)

def visible_material(values,kind):
    # Native VU type 1 reads RGB ambient/diffuse/specular/emission. Its power
    # has no visible effect without specular; material alpha is not vertex alpha.
    if kind==1:return values[0:3]+values[4:7]+values[8:11]+values[12:15]+[values[16] if any(values[8:11]) else 0]
    if kind==4:return values[8:11]+[values[16] if any(values[8:11]) else 0]
    # Types 0/2/5 use packet vertex colours rather than Pat0 material fields.
    return []

def build(job,a):
    parent,e,ids,dump=job;raw=dump.with_name(dump.name.removesuffix('_dump'))
    cabin=getattr(a,'cabin_sources',{}).get(e.get('gt4Model',''))
    if cabin:
        if cabin['directory'] not in ('open','lod','menu'):raise ValueError('Invalid cabin source directory')
        raw=a.vol/'car'/cabin['directory']/e['gt4Model']
        if not raw.is_file():raise ValueError('Missing reviewed cabin source '+str(raw))
        if hashlib.sha256(raw.read_bytes()).hexdigest()!=cabin['sourceSha256']:raise ValueError('Reviewed cabin source changed')
    elif not raw.is_file():
        raw=a.vol/'car'/raw.parent.name/raw.name
    native=a.output/'decode'/parent;dest=a.output/'native-v16'/parent
    native.mkdir(parents=True,exist_ok=True);dest.mkdir(parents=True,exist_ok=True)
    external=raw.with_suffix('.pat')
    if not external.is_file():external=a.vol/'car'/raw.parent.name/(raw.name+'.pat')
    cached=native/'draws.json'
    inputs={'tool':a.tool_sha256,'source':hashlib.sha256(raw.read_bytes()).hexdigest(),
            'pat':hashlib.sha256(external.read_bytes()).hexdigest() if external.is_file() else None}
    stamp=native/'inputs.json'
    try:previous_inputs=json.loads(stamp.read_text())
    except (OSError,ValueError):previous_inputs=None
    try:version=json.loads(cached.read_text()).get('version')
    except (OSError,ValueError):version=0
    if version!=8 or previous_inputs!=inputs:
        r=subprocess.run([str(a.tool),str(raw),str(native),str(external)],capture_output=True,text=True)
        if r.returncode:raise RuntimeError(parent+': '+r.stderr)
        write(stamp,inputs)

    meta=json.loads((native/'draws.json').read_text())
    fallback=None
    # Some locked special cars have a placeholder race body (only frame/lights).
    # Use their complete authored menu body when it has substantially more real
    # geometry; retain the same source units/alignment and original draw states.
    race_triangles=sum(len(d['vertices'])//3 for state in meta['states'] for d in state)
    menu=a.vol/'car'/'menu'/raw.name
    if raw.parent.name=='lod' and race_triangles<1200 and menu.is_file():
        menu_native=a.output/'decode'/(parent+'-menu')
        menu_native.mkdir(parents=True,exist_ok=True)
        menu_cache=menu_native/'draws.json'
        try:menu_version=json.loads(menu_cache.read_text()).get('version')
        except (OSError,ValueError):menu_version=0
        if menu_version!=8:
            r=subprocess.run([str(a.tool),str(menu),str(menu_native)],capture_output=True,text=True)
            if r.returncode:raise RuntimeError(parent+': '+r.stderr)
        menu_meta=json.loads(menu_cache.read_text())
        menu_triangles=sum(len(d['vertices'])//3 for state in menu_meta['states'] for d in state)
        if menu_triangles>race_triangles*2:
            fallback={'reason':'placeholder race body','raceTriangles':race_triangles,'menuTriangles':menu_triangles}
            raw=menu;native=menu_native;meta=menu_meta
    if cabin and cabin.get('donor'):
        donor=cabin['donor'];donor_raw=a.vol/'car'/donor['directory']/e['gt4Model']
        if hashlib.sha256(donor_raw.read_bytes()).hexdigest()!=donor['sourceSha256']:raise ValueError('Cabin donor source changed')
        donor_native=a.output/'decode'/(parent+'-cabin');donor_native.mkdir(parents=True,exist_ok=True)
        r=subprocess.run([str(a.tool),str(donor_raw),str(donor_native)],capture_output=True,text=True)
        if r.returncode:raise RuntimeError(parent+': '+r.stderr)
        donor_meta=json.loads((donor_native/'draws.json').read_text())
        def copy_texture(old,new):write_bytes(native/f'{new[0]}.{new[1]}.rgba',(donor_native/f'{old[0]}.{old[1]}.rgba').read_bytes())
        meta=merge_cabin(meta,donor_meta,donor,copy_texture)
    all_draws=resolve_draws(meta['states'])
    if cabin and cabin.get('glazingPackets'):
        all_draws=split_exterior_glazing(all_draws,cabin['glazingPackets'])
    # The old OBJ export's packed-position unit differs from the native parser.
    # Reuse the cross-checked scale/offset derived from the complete body.
    previous=json.loads(a.alignment.read_text());align=next(r for r in previous if r['parent']==parent)
    sc=align['nativeScale'];offset=align['nativeOffset']
    old=json.loads((a.pack/e['mesh']).read_text())
    old_cockpit=json.loads((a.pack/e['cockpitMesh']).read_text())
    rear=set()
    stats=json.loads((a.pack/parent/'conversion.json').read_text())
    for p,s in ([] if cabin else zip(old_cockpit['meshes'][0]['primitives'],stats['cockpit']['shapes'])):
        if old_cockpit['materials'][p['material']].get('extras',{}).get('gt2pc',{}).get('rearView'):
            rear.add(int(re.match(r'shape(\d+)',s['shape'])[1]))
    gltf={'asset':{'version':'2.0','generator':'owned GT4 native CAR4 draw importer'},'scene':0,
        'scenes':[{'nodes':[0]}],'nodes':[{'mesh':0}],'meshes':[{'primitives':[]}],
        'materials':[],'images':[],'textures':[],'samplers':[{'wrapS':33071,'wrapT':33071}],
        'buffers':[],'bufferViews':[],'accessors':[]}
    binary=bytearray();textures={};material_sources=[];addressing={}
    def decode(file,address,tcc):
        data=file.read_bytes();w,h=struct.unpack("<II",data[:8]);im=np.frombuffer(data[8:],dtype=np.uint8).reshape(h,w,4).copy()
        im[:,:,3]=np.minimum(im[:,:,3].astype(np.uint16)*2,255).astype(np.uint8) if tcc else 255
        u,v,loU,hiU,loV,hiV=address
        xs=np.arange(w);ys=np.arange(h)
        if u==2:xs=np.clip(xs,loU,hiU)
        elif u==3:xs=(xs&loU)|hiU
        if v==2:ys=np.clip(ys,loV,hiV)
        elif v==3:ys=(ys&loV)|hiV
        if xs.max()>=w or ys.max()>=h:raise ValueError("GS address exceeds allocated texture")
        return Image.fromarray(im[ys[:,None],xs[None,:]],"RGBA")
    def acc(v,width):
        at=len(binary)
        for row in v:binary.extend(struct.pack('<'+'f'*width,*row))
        view=len(gltf['bufferViews']);gltf['bufferViews'].append({'buffer':0,'byteOffset':at,'byteLength':len(binary)-at})
        i=len(gltf['accessors']);gltf['accessors'].append({'bufferView':view,'componentType':5126,'count':len(v),'type':'VEC'+str(width)})
        return i
    # Full callback masks retain on/off material substitutions, not just shapes
    # absent in the off state. Original environment geometry remains independent.
    reflection_vertices=glass=triangles=0
    default=[1,1,1,1]*2+[0,0,0,1]*2+[0]
    mats=meta['materials'][0]
    for d,mask in all_draws:
        v=d['vertices']
        if not v:continue
        pbr={'baseColorFactor':[1,1,1,1],'metallicFactor':0,'roughnessFactor':1}
        if d['texture']>=0:
            tkey=d['texset'],d['texture']
            if tkey not in textures:
                address=d['address'];tcc=d['textureAlpha'];addressing[tkey]=(address,tcc)
                im=decode(native/f'{tkey[0]}.{tkey[1]}.rgba',address,tcc)
                name=f'textures/{tkey[0]}.{tkey[1]}.png';(dest/'textures').mkdir(exist_ok=True)
                base=a.pack/parent/name
                original=Image.open(base).convert('RGBA') if base.is_file() else None
                if original and original.size==im.size and original.tobytes()==im.tobytes():uri='../../'+parent+'/'+name
                else:
                    target=dest/name
                    existing=Image.open(target).convert('RGBA') if target.is_file() else None
                    if existing is None or existing.size!=im.size or existing.tobytes()!=im.tobytes():
                        temporary=target.with_name(target.name+'.tmp');im.save(temporary,format='PNG');temporary.replace(target)
                    uri=name
                textures[tkey]=len(gltf['images']);gltf['images'].append({'uri':uri,'extras':{'paintKey':name}})
                sampler=len(gltf['samplers']);gltf['samplers'].append({'wrapS':10497 if address[0] in (0,3) else 33071,'wrapT':10497 if address[1] in (0,3) else 33071})
                gltf['textures'].append({'source':textures[tkey],'sampler':sampler})
            pbr['baseColorTexture']={'index':textures[tkey]}
        m=d['material'];values=mats[m] if 0<=m<len(mats) else default
        native_mat={'type':d['type'],'values':values,'alphaFunc':d['alphaFunc'],'alphaRef':d['alphaRef'],
            'textureFunction':d.get('textureFunction',0),'textureAlpha':d.get('textureAlpha',0),'alphaFail':d['alphaFail'],'cull':d['cull'],'blendEquation':d['blendEquation'],'depthWrite':d['depthWrite']}
        env=d['type'] in (2,4);blend=d['blend'] and d['blendEquation']==0x44
        if d['blend'] and d['blendEquation'] not in (0x44,0x58,0x48):
            raise ValueError(f'Unsupported native blend {parent}: {d["blendEquation"]:x}')
        mi=len(gltf['materials']);material_sources.append(m)
        gltf['materials'].append({'name':f'native-{d["shape"]}-{d["packet"]}',
            'alphaMode':'BLEND' if blend else 'MASK','doubleSided':not d['cull'],'pbrMetallicRoughness':pbr,
            'extras':{'gt2pc':{'nativeGt4':native_mat,'reflection':env,'rearView':d['shape'] in rear,
                'interiorOnly':d.get('interiorOnly',False),'exteriorOnly':d.get('exteriorOnly',False),
                'cockpitControl':bool(cabin and [d['shape'],d['packet']] in cabin.get('controls',[])),
                'lampStates':0 if mask==63 else mask,'wheelAttachment':d['wheel'],
                'wheelSpinning':d['spinning']}}})
        gltf['meshes'][0]['primitives'].append({'attributes':{
            'POSITION':acc([[q[k]*sc+(offset[k] if d['wheel']<0 else 0) for k in range(3)] for q in v],3),
            'NORMAL':acc([q[3:6] for q in v],3),'TEXCOORD_0':acc([q[6:8] for q in v],2),
            'COLOR_0':acc([q[8:12] for q in v],4)},'material':mi,'mode':4})
        if env:reflection_vertices+=len(v)
        if blend:glass+=1
        triangles+=len(v)//3
    gltf['buffers']=[{'uri':'body.bin.gz','byteLength':len(binary)}]
    write_bytes(dest/'body.bin.gz',gzip.compress(binary,compresslevel=6,mtime=0));write(dest/'body.gltf.gz',gltf)
    # Texture palette entries keep their original source colour. Material-only
    # Pat0 colours must be accessible even if the older RGB export deduplicated them.
    old_palette=json.loads((a.paints/parent/'colors.json').read_text())['colors']
    palettes=[];seen=set()
    source_colors=[-1]+list(range(len(meta['paintMaterials'])))
    if len(source_colors)==1:source_colors=[r['sourceColor'] for r in old_palette]
    for source_color in source_colors:
        row=next((r for r in old_palette if r['sourceColor']==source_color),None)
        material_set=mats if source_color<0 or not meta['paintMaterials'] else meta['paintMaterials'][source_color]
        values=[material_set[m] if 0<=m<len(material_set) else default for m in material_sources]
        images={}
        if source_color>=0 and meta['paintImages']:
            for name,file in meta['paintImages'][source_color].items():
                tkey=tuple(map(int,name.split('.')));address,tcc=addressing[tkey]
                im=decode(native/file,address,tcc);digest=hashlib.sha256(im.tobytes()).hexdigest()
                directory=dest/'paints';directory.mkdir(exist_ok=True);target=directory/(digest+'.png')
                # Equal RGBA byte streams can have different image dimensions
                # (e.g. uniform 32x128 vs 64x64). Never reuse that PNG layout.
                if target.is_file() and Image.open(target).size!=im.size:
                    digest=hashlib.sha256(struct.pack('<II',*im.size)+im.tobytes()).hexdigest()
                    target=directory/(digest+'.png')
                if not target.is_file():im.save(target)
                images['textures/'+name+'.png']=parent+'/paints/'+target.name

        signature=json.dumps([images,[visible_material(v,material['extras']['gt2pc']['nativeGt4']['type']) for v,material in zip(values,gltf['materials'])]],sort_keys=True,separators=(',',':'))
        if signature in seen:continue
        seen.add(signature)
        chip=row.get('chip',0x808080) if row else 0x808080
        if not row:
            changed=[v[4:7] for v,base in zip(values,[mats[m] if 0<=m<len(mats) else default for m in material_sources]) if v!=base]
            if changed:
                color=max(changed,key=lambda c:max(c)-min(c));c=[round(255*max(0,min(1,v))) for v in color]
                chip=c[0]|c[1]<<8|c[2]<<16
        palettes.append({'sourceColor':source_color,'images':images,'chip':chip,'materials':values})
    write(dest/'paints.json.gz',{'format':'gt4-native-paints','version':1,'colors':palettes})
    return {'parent':parent,'ids':ids,'triangles':triangles,'reflectionVertices':reflection_vertices,'glassPrimitives':glass,
        'materials':len(material_sources),'paints':len(palettes),'rawSha256':hashlib.sha256(raw.read_bytes()).hexdigest(),
        'source':str(raw),'nativeScale':sc,'nativeOffset':offset,'sourceFallback':fallback,'cabinSource':cabin}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('pack','paints','output','tool','alignment','vol'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--cars',nargs='+');p.add_argument('--workers',type=int,default=4)
    p.add_argument('--cabin-sources',type=Path,default=Path(__file__).resolve().parents[1]/'db/gt4-native-interiors.json',help='Hash-bound reviewed source decisions keyed by GT4 code')
    a=p.parse_args()
    a.cabin_sources=json.loads(a.cabin_sources.read_text())['cars'] if a.cabin_sources else {}
    for name in ('pack','paints','output','tool','alignment','vol'):setattr(a,name,getattr(a,name).resolve())
    a.tool_sha256=hashlib.sha256(a.tool.read_bytes()).hexdigest()
    if not a.output.is_relative_to(Path('work').resolve()):p.error('Output must be under work')
    manifest=json.loads((a.pack/'manifest.json').read_text())['cars']
    # Alignment records also bind each model to its original CAR4 input. This
    # supports a release pack whose earlier OBJ intermediate was archived.
    alignment=json.loads(a.alignment.read_text())
    dumps={m['parent']:Path(m['source']+'_dump') for m in alignment};jobs={}
    for id,e in manifest.items():
        if a.cars and id not in a.cars:continue
        code=e.get('gt4Model','')
        if (a.vol/'car/open'/code).is_file() and not e.get('nativeInterior') and code not in a.cabin_sources:
            raise ValueError('Authored open source has no reviewed interior decision: '+code)
        if code in a.cabin_sources and not all(k in a.cabin_sources[code] for k in ('eye','mirror','openTop')):
            raise ValueError('Incomplete reviewed cabin profile: '+code)
        parent=Path(e['mesh']).parent.as_posix();jobs.setdefault(parent,[parent,e,[],dumps[parent]])[2].append(id)
    report=[];index={'format':'gt4-native-overlay','version':1,'cars':{}}
    with concurrent.futures.ThreadPoolExecutor(a.workers) as pool:
        for r in pool.map(lambda j:build(j,a),jobs.values()):
            report.append(r)
            for id in r['ids']:index['cars'][id]=r['parent']+'/body.gltf.gz'
            if len(report)%20==0:print(f'Native cars {len(report)}/{len(jobs)}',flush=True)
    write(a.output/'native-v16'/'manifest.json',index);write(a.output/'report.json',report)
    cabins={'format':'gt4-native-interiors','version':1,'cars':{}}
    for row in report:
        decision=row['cabinSource']
        if not decision or 'eye' not in decision:continue
        for id in row['ids']:
            cabins['cars'][id]={'mesh':index['cars'][id],'nativeInterior':True,**{k:decision[k] for k in ('openTop','eye','mirror','speedometer') if k in decision}}
    write(a.output/'native-v16'/'interiors.json',cabins)
    print(json.dumps({'cars':len(index['cars']),'models':len(report),'triangles':sum(r['triangles'] for r in report),
        'paints':sum(r['paints'] for r in report)}),flush=True)
if __name__=='__main__':main()
