"""Export authored Pat0 colours as an additive overlay for an immutable GT4 pack.

Uses the native gt4tool Tex1 decoder. Alpha masks, meshes and UVs stay
byte-identical. Only RGB texels actually changed by a validated paint patch are
replaced. Original data and every decoder output remain under work.
"""
import argparse, concurrent.futures, hashlib, json, re, shutil, struct, subprocess
from pathlib import Path
import numpy as np
from PIL import Image

def u32(data, at): return struct.unpack_from('<I', data, at)[0]
def sha(data): return hashlib.sha256(data).hexdigest()
def patch(data, at, color, body, size):
    if data[at:at+4] != b'Pat0': raise ValueError('Invalid Pat0')
    colors, entries = struct.unpack_from('<HH', data, at+16)
    if not 0 <= color < colors: raise ValueError('Invalid paint index')
    result = bytearray(data)
    for i in range(entries):
        p = u32(data, at+32+4*(color*entries+i))
        if not p: continue
        dest, length = struct.unpack_from('<II', data, at+p)
        if dest+length > size or at+p+8+length > len(data): raise ValueError('Paint patch escapes model')
        result[body+dest:body+dest+length] = data[at+p+8:at+p+8+length]
    return result

def build(job, a):
    parent, entry, ids, dump = job
    raw = dump.with_name(dump.name.removesuffix('_dump')) if dump else None
    if raw is None or not raw.is_file():
        directory = 'open' if parent.endswith('-open') else 'lod'
        raw = a.vol/'car'/directory/entry['gt4Model']
    data = raw.read_bytes()
    if data[:4] != b'CAR4': raise ValueError('Missing CAR4: '+str(raw))
    body, pat = u32(data,24), u32(data,28)
    external = raw.with_suffix('.pat')
    if not external.is_file(): external = a.vol/'car'/raw.parent.name/(raw.name+'.pat')
    if not pat and external.is_file():
        pat = len(data); data += external.read_bytes()
    colors = struct.unpack_from('<H', data, pat+16)[0] if pat else 0
    size = u32(data,body+16)
    table = u32(data,body+0x44); lst = u32(data,body+table)
    sets = [body+u32(data,body+lst+4*i) for i in range(struct.unpack_from('<H',data,body+0x1c)[0])]
    images = {}
    for field in ['mesh','cockpitMesh']:
        mesh = a.pack/entry[field]; doc = json.loads(mesh.read_text())
        for im in doc.get('images',[]):
            key = im['uri']; match = re.fullmatch(r'textures/(\d+)\.(\d+)\.png',key)
            if not match: raise ValueError('Unrecognized image: '+key)
            images[key] = (int(match[1]),int(match[2]),np.array(Image.open(mesh.parent/key).convert('RGBA')))
    dest = a.output/parent; dest.mkdir(parents=True,exist_ok=True)
    temp = a.output/'_decode'/parent; temp.mkdir(parents=True,exist_ok=True)
    baseline = {}
    for c in range(-1, colors):
        patched = data if c < 0 else patch(data,pat,c,body,size)
        file = temp/'model'; file.write_bytes(patched)
        for setid in sorted({v[0] for v in images.values()}):
            target = temp/str(setid)
            r = subprocess.run([str(a.tool),'tex',str(file.resolve()),str(target.resolve()),'--offset',str(sets[setid])],
                stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=30)
            if r.returncode: raise ValueError(r.stdout)
        decoded = {}
        for key,(setid,index,original) in images.items():
            z = np.array(Image.open(temp/str(setid)/f'model_{index}.png').convert('RGBA'))
            # The importer keeps the used top-left CLAMP rectangle.
            z = z[:original.shape[0],:original.shape[1]]
            if z.shape != original.shape: raise ValueError('Image dimensions changed')
            decoded[key] = z
        if c < 0:
            baseline = decoded
            for key,(_,_,original) in images.items():
                if not np.array_equal(original[:,:,:3],baseline[key][:,:,:3]):
                    raise ValueError('Decoder RGB differs from original: '+parent+'/'+key)
            palettes = [{'sourceColor':-1,'images':{}}]; seen = {()}; changes = []
            continue
        replacements = {}; signature = []; sample = []
        for key,(_,_,original) in images.items():
            changed = np.any(decoded[key][:,:,:3]!=baseline[key][:,:,:3],axis=2)
            if not np.any(changed): continue
            rgba = original.copy();rgba[changed,:3] = decoded[key][changed,:3]
            digest = sha(rgba.tobytes()); name = digest+'.png'
            if not (dest/name).exists(): Image.fromarray(rgba).save(dest/name)
            replacements[key] = (Path(parent)/name).as_posix();signature.append((key,digest))
            sample.extend(rgba[changed,:3][::max(1,np.count_nonzero(changed)//64)].tolist())
            changes.extend(original[changed,:3][::max(1,np.count_nonzero(changed)//64)].tolist())
        signature = tuple(sorted(signature))
        if signature in seen: continue
        seen.add(signature)
        rgb = np.median(sample,axis=0).astype(int) if sample else np.array([128]*3)
        palettes.append({'sourceColor':c,'chip':int(rgb[0])|int(rgb[1])<<8|int(rgb[2])<<16,'images':replacements})
    if not colors: palettes = [{'sourceColor':-1,'images':{}}];changes=[]
    rgb = np.median(changes,axis=0).astype(int) if changes else np.array([128]*3)
    palettes[0]['chip'] = int(rgb[0])|int(rgb[1])<<8|int(rgb[2])<<16
    result = {'format':'gt4-paints','version':1,'colors':palettes}
    (dest/'colors.json').write_text(json.dumps(result,separators=(',',':'))+'\n')
    shutil.rmtree(temp)  # temp is the verified output/_decode/parent directory
    return {'parent':parent,'ids':ids,'colors':len(palettes),'patchColors':colors,'rawSha256':sha(raw.read_bytes()),
        'source':str(raw),'textureTexels':sum(x[2].shape[0]*x[2].shape[1] for x in images.values()),
        'variantTexels':sum(Image.open(q).width*Image.open(q).height for q in dest.glob('*.png'))}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for field in ['pack','output','vol','tool']:p.add_argument('--'+field,type=Path,required=True)
    p.add_argument('--cars',nargs='+');p.add_argument('--workers',type=int,default=4);a=p.parse_args()
    a.output = a.output.resolve();a.tool=a.tool.resolve();a.pack=a.pack.resolve();a.vol=a.vol.resolve()
    if not a.output.is_relative_to(Path('work').resolve()) or a.output.exists():p.error('Choose a fresh output under work')
    manifest=json.loads((a.pack/'manifest.json').read_text());provenance=json.loads((a.pack/'provenance.json').read_text())
    dumps={Path(j['mesh']).parent.as_posix():Path(j['dump']) for j in provenance['models']}
    jobs={}
    for id,e in manifest['cars'].items():
        if a.cars and id not in a.cars:continue
        parent=Path(e['mesh']).parent.as_posix();jobs.setdefault(parent,[parent,e,[],dumps.get(parent)])[2].append(id)
    a.output.mkdir(parents=True);report=[];index={'format':'gt4-paint-overlay','version':1,'cars':{}}
    with concurrent.futures.ThreadPoolExecutor(a.workers) as pool:
        for r in pool.map(lambda j:build(j,a),jobs.values()):
            report.append(r)
            for id in r['ids']:index['cars'][id]=r['parent']+'/colors.json'
            if len(report)%20==0:print('Paints',len(report),'/',len(jobs),flush=True)
    (a.output/'manifest.json').write_text(json.dumps(index,indent=2)+'\n')
    (a.output/'report.json').write_text(json.dumps({'sourceManifestSha256':sha((a.pack/'manifest.json').read_bytes()),'models':report},indent=2)+'\n')
    print('Ready',len(index['cars']),'cars,',len(report),'meshes',flush=True)
if __name__=='__main__':main()
