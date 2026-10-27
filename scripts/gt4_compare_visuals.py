"""Rank possible geometry holes and assemble a private GT4/PCSX2 review gallery.

Requires Pillow and NumPy. Cyan background components inside a silhouette
are candidates for review, not automatic defects: open cockpits and grilles can
contain legitimate gaps. PS2 lighting/cameras differ, so no pixel-match verdict
is fabricated. Reference identities must be checked in the original game menu.
"""
import argparse
import hashlib
import html
import json
import os
from pathlib import Path
from PIL import Image,ImageDraw
import numpy as np


def background_components(mask):
    # Scanline components avoid a dependency on SciPy/OpenCV and do not walk
    # every background pixel in Python. Connectivity is horizontal/vertical.
    height,width=mask.shape;parents=[];runs=[];previous=[]
    def root(i):
        while parents[i]!=i:
            parents[i]=parents[parents[i]];i=parents[i]
        return i
    for y,row in enumerate(mask):
        edges=np.flatnonzero(np.diff(np.r_[False,row,False]))
        current=[];offset=0
        for start,stop in zip(edges[::2],edges[1::2]):
            start,stop=int(start),int(stop);index=len(parents);parents.append(index)
            runs.append((y,start,stop));current.append((start,stop,index))
            while offset<len(previous) and previous[offset][1]<=start:offset+=1
            j=offset
            while j<len(previous) and previous[j][0]<stop:
                a,b=root(index),root(previous[j][2])
                if a!=b:parents[a]=b
                j+=1
        previous=current
    components={}
    for i,(y,start,stop) in enumerate(runs):
        key=root(i);c=components.setdefault(key,{'pixels':0,'box':[width,height,0,0],'border':False})
        c['pixels']+=stop-start;c['border']|=y in (0,height-1) or start==0 or stop==width
        box=c['box'];box[0]=min(box[0],start);box[1]=min(box[1],y);box[2]=max(box[2],stop);box[3]=max(box[3],y+1)
    return [dict(pixels=c['pixels'],box=c['box']) for c in components.values() if not c['border'] and c['pixels']>=20]


def sha(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()


def inspect(path):
    im=Image.open(path).convert('RGB');pixels=np.asarray(im).astype(np.int16)
    background=np.max(np.abs(pixels-pixels[0,0]),axis=2)<=3
    foreground=~background
    holes=background_components(background)
    ys,xs=np.where(foreground)
    crop=bool(np.count_nonzero(foreground[0])+np.count_nonzero(foreground[-1])+
              np.count_nonzero(foreground[:,0])+np.count_nonzero(foreground[:,-1])>8)
    return {'foregroundPixels':int(foreground.sum()),'empty':bool(foreground.sum()<im.width*im.height*.01),
            'cropped':crop,'holes':holes,'holePixels':sum(h['pixels'] for h in holes),
            'bounds':[int(xs.min()),int(ys.min()),int(xs.max()+1),int(ys.max()+1)] if xs.size else None}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--audit',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--references',type=Path,help='JSON list: id, name, identityVerified, frames (absolute paths), optional crop')
    p.add_argument('--resume',action='store_true',help='Reuse unchanged frames from an existing report')
    a=p.parse_args();work=Path(__file__).resolve().parents[1]/'work'
    out=a.output.resolve()
    if not out.is_relative_to(work) or (out.exists() and not a.resume):p.error('Choose a fresh output under work, or use --resume')
    out.mkdir(exist_ok=a.resume);(out/'suspects').mkdir(exist_ok=a.resume);(out/'references').mkdir(exist_ok=a.resume)
    cached={}
    if a.resume and (out/'report.json').exists():
        previous=json.loads((out/'report.json').read_text())
        if previous['audit']!=str(a.audit.resolve()):raise ValueError('Resume audit path differs')
        cached={r['id']:r for r in previous['carsReport']}
    rows=json.loads((a.audit/'report.json').read_text());refs={}
    if a.references:
        for ref in json.loads(a.references.read_text()):
            if not ref.get('identityVerified'):raise ValueError('Unverified PS2 identity: '+ref['id'])
            paths=[]
            for i,path in enumerate(ref['frames']):
                source=Path(path);dest=out/'references'/(ref['id']+'-'+str(i)+'.png')
                im=Image.open(source)
                if ref.get('crop'):im=im.crop(ref['crop'])
                im.save(dest);paths.append({'path':dest.relative_to(out).as_posix(),'sourceSha256':sha(source)})
            refs[ref['id']]=dict(ref,frames=paths)
    analysed=[]
    for row in rows:
        frames=[]
        if row['captureComplete']:
            old={f['file']:f for f in cached.get(row['id'],{}).get('frames',[])}
            for file in row['images']:
                path=a.audit/'frames'/file;digest=sha(path)
                if file in old and old[file].get('sha256')==digest:
                    frames.append(old[file]);continue
                result=inspect(path);result['file']=file;result['sha256']=digest
                if result['holes']:
                    im=Image.open(path).convert('RGB');draw=ImageDraw.Draw(im)
                    for hole in result['holes']:draw.rectangle(hole['box'],outline='#ff00ff',width=2)
                    im.save(out/'suspects'/file)
                frames.append(result)
        bad=not row['captureComplete'] or any(f['empty'] or f['cropped'] for f in frames)
        analysed.append(dict(row,frames=frames,captureValid=not bad,
                             candidateHolePixels=sum(f['holePixels'] for f in frames),
                             referenceAvailable=row['id'] in refs,
                             ps2PixelMatched=False))
        if len(analysed)%100==0:print('Analysed',len(analysed),flush=True)
    analysed.sort(key=lambda r:(r['captureValid'],-r['candidateHolePixels']))
    report={'audit':str(a.audit.resolve()),'cars':len(analysed),'invalidCaptures':[r['id'] for r in analysed if not r['captureValid']],
            'references':refs,'automaticDefectVerdict':False,'carsReport':analysed}
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    cards=[]
    for row in analysed:
        images=[]
        for frame in row['frames']:
            path=out/'suspects'/frame['file'] if frame['holes'] else a.audit/'frames'/frame['file']
            # Relative paths keep the gallery portable inside the private work tree.
            src=Path(os.path.relpath(path,out)).as_posix()
            images.append(f'<a href="{html.escape(src)}"><img loading="lazy" src="{html.escape(src)}" title="{frame["file"]}: {frame["holePixels"]} candidate pixels"></a>')
        reference='<p>Original GT4 reference not captured; not compared.</p>'
        if row['id'] in refs:
            reference='<p>Original GT4 / PCSX2: '+html.escape(refs[row['id']]['name'])+'</p>'+''.join(
                '<img loading="lazy" src="'+f['path']+'">' for f in refs[row['id']]['frames'])
        cockpit=a.audit/'frames'/(row['id']+'-cockpit.png')
        cockpit_panel=''
        if cockpit.exists():
            cockpit_src=Path(os.path.relpath(cockpit,out)).as_posix()
            cockpit_panel='<p>Seated cockpit (glass exclusion is specific to this mesh)</p><img loading="lazy" src="'+html.escape(cockpit_src)+'">'
        cards.append('<details data-name="'+html.escape(row['id']+' '+row['name'])+'"><summary>'+html.escape(row['id']+' '+row['name'])+
                     f' — candidate pixels {row["candidateHolePixels"]}, capture valid {row["captureValid"]}</summary>'+reference+
                     '<p>GT2 production renderer</p><div>'+''.join(images)+'</div>'+cockpit_panel+'</details>')
    page='''<!doctype html><meta charset="utf-8"><title>GT4 visual comparison</title>
<style>body{background:#20232a;color:#eee;font:16px system-ui;margin:24px}img{width:300px;max-width:45vw}details{margin:12px 0;padding:12px;background:#303641}summary{cursor:pointer}input{padding:12px;width:400px}</style>
<h1>GT4 geometry review</h1><p>Cyan shows background. Magenta boxes mark enclosed gaps for inspection. These can be legitimate openings.
PS2 frames are visual references; lighting, paint, wheels and camera poses differ. Capture success does not establish headset acceptance.</p>
<input id="search" placeholder="Car name or ID"><p>'''+str(len(analysed))+' cars; '+str(len(report['invalidCaptures']))+' invalid captures; '+str(len(refs))+' original PS2 references.</p>'+''.join(cards)+'''
<script>search.oninput=()=>document.querySelectorAll('details').forEach(x=>x.hidden=!x.dataset.name.toLowerCase().includes(search.value.toLowerCase()))</script>'''
    (out/'index.html').write_text(page,encoding='utf-8')
    print('Ready',len(analysed),'cars;',len(report['invalidCaptures']),'invalid captures',flush=True)
    if report['invalidCaptures']:raise SystemExit(1)


if __name__=='__main__':main()
