"""Render every newly selected authored cabin and reproduce the old spoke bug."""
import argparse,concurrent.futures,csv,gzip,hashlib,json,os,shutil,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('disc','pack','native','expansion','output'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--build',type=Path,default=ROOT/'build_gt4_models');p.add_argument('--workers',type=int,default=6)
    a=p.parse_args();out=a.output.absolute();out.relative_to(ROOT/'work');out.mkdir(exist_ok=False)
    exe=out/'gt2interioraudit.exe';shutil.copy2(a.build/'gt2interioraudit.exe',exe)
    cabins=json.loads((a.native/'interiors.json').read_text());overlay=json.loads((a.native/'manifest.json').read_text())
    if cabins.get('format')!='gt4-native-interiors' or cabins.get('version')!=1:raise ValueError('Invalid interior catalogue')
    ids=sorted(cabins['cars'])
    if not ids:raise ValueError('No discovered cabins selected')
    for id in ids:
        row=cabins['cars'][id]
        if not row['nativeInterior'] or row['mesh']!=overlay['cars'][id]:raise ValueError('Interior selection mismatch: '+id)
    env={k:v for k,v in os.environ.items() if not k.startswith('GT2_')}
    env.update(GT2_AUDIT_COCKPIT='1',GT2_GT4_NATIVE=str(a.native.absolute()),GT2_CAR_EXPANSION=str(a.expansion.absolute()),GT2_WINDOW='hidden',GT2_NO_FOCUS='1')
    def run(id,negative=False):
        folder=out/('legacy-'+id if negative else id);folder.mkdir();local=dict(env)
        if negative:local['GT2_AUDIT_LEGACY_ALPHA_FAIL']='1'
        with (folder/'render.log').open('w') as log:r=subprocess.run([str(exe),str(a.disc.absolute()),str(a.pack.absolute()),str(folder),id],cwd=ROOT,env=local,stdout=log,stderr=subprocess.STDOUT,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        return id,r.returncode
    if 'z1030' not in ids:raise ValueError('Missing owned alpha-fail regression witness')
    _,rc=run('z1030',True)
    negative=out/'legacy-z1030/render.log'
    if rc!=1 or 'authored steering cutouts became opaque' not in negative.read_text():raise ValueError('Legacy alpha-fail defect was not reproduced')
    # Compare actual open pixels against the authored geometry. A height-over-
    # bonnet metric alone cannot detect filling intentionally open chassis gaps.
    folder=out/'legacy-enclosure';folder.mkdir()
    local=dict(env,GT2_AUDIT_LEGACY_NATIVE_ENCLOSURE='1')
    with (folder/'render.log').open('w') as log:
        result=subprocess.run([str(exe),str(a.disc.absolute()),str(a.pack.absolute()),str(folder),'z0979'],cwd=ROOT,env=local,stdout=log,stderr=subprocess.STDOUT,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    if result.returncode!=1 or 'authored open chassis gaps were enclosed' not in (folder/'render.log').read_text():raise ValueError('Legacy native enclosure was not detected')
    # Recreate the actual SERA donor omission in an isolated, owned-data fixture.
    # Only the reviewed fixed jamb is removed; production assets stay untouched.
    folder=out/'legacy-door-seam';folder.mkdir();fixture=folder/'native';fixture.mkdir()
    original=(a.native/overlay['cars']['z0357']).resolve()
    model=json.loads(gzip.decompress(original.read_bytes()))
    omitted={n for n,m in enumerate(model['materials']) if m['name'] in ('native-7-6','native-7-7','native-7-8') and m.get('extras',{}).get('gt2pc',{}).get('interiorOnly')}
    if len(omitted)<3:raise ValueError('Missing source-bound SERA fixed-jamb witness')
    for mesh in model['meshes']:mesh['primitives']=[p for p in mesh['primitives'] if p['material'] not in omitted]
    for item in model.get('images',[])+model.get('buffers',[]):
        if 'uri' in item and not item['uri'].startswith('data:'):item['uri']=(original.parent/item['uri']).resolve().as_posix()
    (fixture/'body.gltf.gz').write_bytes(gzip.compress(json.dumps(model).encode(),mtime=0))
    palette=json.loads(gzip.decompress((original.parent/'paints.json.gz').read_bytes()))
    for color in palette['colors']:color['images']={}
    (fixture/'paints.json.gz').write_bytes(gzip.compress(json.dumps(palette).encode(),mtime=0))
    (fixture/'manifest.json').write_text(json.dumps({'format':'gt4-native-overlay','version':1,'cars':{'z0357':'body.gltf.gz'}}))
    row=dict(cabins['cars']['z0357'],mesh='body.gltf.gz')
    (fixture/'interiors.json').write_text(json.dumps({'format':'gt4-native-interiors','version':1,'cars':{'z0357':row}}))
    if (a.native/'wheels.json').exists():shutil.copy2(a.native/'wheels.json',fixture/'wheels.json')
    local=dict(env,GT2_GT4_NATIVE=str(fixture),GT2_AUDIT_LEGACY_NATIVE_LINING='1')
    with (folder/'render.log').open('w') as log:
        result=subprocess.run([str(exe),str(a.disc.absolute()),str(a.pack.absolute()),str(folder),'z0357'],cwd=ROOT,env=local,stdout=log,stderr=subprocess.STDOUT,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    if result.returncode!=1 or 'closed native cabin has open door/dashboard seams' not in (folder/'render.log').read_text():raise ValueError('Legacy native door seam was not detected')
    folder=out/'legacy-inner-lining';folder.mkdir();local=dict(env,GT2_AUDIT_LEGACY_NATIVE_LINING='1')
    with (folder/'render.log').open('w') as log:
        result=subprocess.run([str(exe),str(a.disc.absolute()),str(a.pack.absolute()),str(folder),'z0357'],cwd=ROOT,env=local,stdout=log,stderr=subprocess.STDOUT,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    if result.returncode!=1 or 'closed native cabin has open door/dashboard seams' not in (folder/'render.log').read_text():raise ValueError('Legacy native inner-wall culling was not detected')
    with concurrent.futures.ThreadPoolExecutor(a.workers) as pool:results=dict(pool.map(run,ids))
    rows=[]
    for id in ids:rows.extend(csv.DictReader((out/id/'interiors.csv').open()))
    if len(rows)!=len(ids) or any(r['native']!='1' or int(r['triangles'])==0 for r in rows):raise ValueError('Incomplete authored interior coverage')
    digest=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
    report={'status':'failed' if any(results.values()) else 'passed','paths':len(ids),'stereo_views_per_path':9,'exit_codes':results,'legacy_alpha_failed':True,'legacy_enclosure_failed':True,'legacy_door_seam_failed':True,'legacy_inner_lining_failed':True,'exe_sha256':digest(exe),'interiors_sha256':digest(a.native/'interiors.json'),'overlay_sha256':digest(a.native/'manifest.json')}
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report),flush=True)
    return int(any(results.values()))
if __name__=='__main__':raise SystemExit(main())
