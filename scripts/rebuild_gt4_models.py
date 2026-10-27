"""Rebuild an existing private GT4 manifest using cached, owned OBJ dumps.

The source pack is immutable. Missing cached dumps can be extracted from the
verified owned disc into a separate cache. No downloads are performed.
"""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path
from gt4_mesh import convert


def digest(path):
    with path.open('rb') as source: return hashlib.file_digest(source, 'sha256').hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--pack',type=Path,required=True)
    p.add_argument('--dump-root',type=Path,action='append',required=True)
    p.add_argument('--profiles',type=Path,default=Path(__file__).with_name('gt4_profiles.json'))
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--cars',nargs='+')
    p.add_argument('--iso',type=Path,help='Owned source disc, only for missing cached dumps')
    p.add_argument('--tool',type=Path,help='Existing source-built model dumper')
    p.add_argument('--dump-cache',type=Path)
    a=p.parse_args()
    if a.output.exists():p.error('Output already exists; choose a fresh directory')
    manifest=json.loads((a.pack/'manifest.json').read_text())
    profiles=json.loads(a.profiles.read_text())['cars']
    if a.cars:
        manifest['cars']={i:e for i,e in manifest['cars'].items() if i in a.cars}
        if len(manifest['cars'])!=len(a.cars):p.error('Unknown or repeated car')
    jobs={}
    missing={}
    for id,entry in manifest['cars'].items():
        parent=Path(entry['mesh']).parent.as_posix()
        profile=profiles.get(parent,{})
        directory='open' if parent.endswith('-open') else profile.get('directory','lod')
        code=entry['gt4Model']
        dumps=[root/directory/(code+'_dump') for root in a.dump_root]
        dump=next((d for d in dumps if (d/'model0.obj').exists() or (d/'model0.lod0.obj').exists()),None)
        if dump is None:
            if not (a.iso and a.tool and a.dump_cache):raise ValueError('Cached dump missing: '+directory+'/'+code)
            dump=a.dump_cache/directory/(code+'_dump')
            missing[directory+'/'+code]=dump
        job={'dump':dump,'entry':entry,'profile':profile,'ids':[]}
        if parent in jobs and jobs[parent]['dump']!=dump:raise ValueError('Conflicting resource: '+parent)
        jobs.setdefault(parent,job)['ids'].append(id)
    if missing:
        from gt4_iso import inventory
        from import_gt4_models import extract
        with a.iso.open('rb') as source:
            inv=inventory(source)
            expected=json.loads(a.profiles.read_text())['discTocSha256']
            if inv['toc_fingerprint_sha256']!=expected:raise ValueError('Unverified source disc revision')
            entries={e['path']:e for e in inv['entries'] if e['kind']}
            for index,(resource,dump) in enumerate(missing.items()):
                if (dump/'model0.obj').exists() or (dump/'model0.lod0.obj').exists():continue
                raw=dump.with_name(dump.name.removesuffix('_dump'));raw.parent.mkdir(parents=True,exist_ok=True)
                raw.write_bytes(extract(source,entries['car/'+resource]))
                with raw.with_suffix('.log').open('w') as log:
                    subprocess.run(['dotnet',str(a.tool.resolve()),'dump','-i',str(raw.resolve())],
                                   stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120)
                print(f'Decoded missing source {index+1}/{len(missing)}: {resource}',flush=True)
    a.output.mkdir(parents=True)
    report=[]
    for index,(parent,job) in enumerate(jobs.items()):
        profile=job['profile'];entry=job['entry'];dump=job['dump']
        if not (dump/'texture_addressing.json').is_file():
            raise ValueError('Missing TEX0/CLAMP metadata; dump with adapter revision 11: '+str(dump))
        stats={}
        for name,key,seated in [('exterior','mesh',False),('cockpit','cockpitMesh',True)]:
            includes=(profile.get('cockpitIncludeShapes',profile.get('includeShapes',[]) if profile.get('nativeInterior') else [])
                      if seated else profile.get('includeShapes',[]))
            stats[name]=convert(dump,a.output/entry[key],cockpit=seated,
                cockpit_skip_textures=[f'0.{i}.png' for i in profile.get('cockpitSkipTextures',[])],
                include_shapes=includes,
                opaque_textures=[f'0.{i}.png' for i in profile.get('opaqueTextures',[])],
                mirror_textures=[f'0.{i}.png' for i in profile.get('mirrorTextures',[])])
        if 3*sum(x['triangles'] for x in stats.values())>65536:raise ValueError('Vertex budget: '+parent)
        (a.output/parent/'conversion.json').write_text(json.dumps(stats,indent=2)+'\n')
        obj=dump/('model0.obj' if (dump/'model0.obj').exists() else 'model0.lod0.obj')
        report.append({'ids':job['ids'],'dump':str(dump),'objSha256':digest(obj),
                       'commandsSha256':digest(dump/'model_set.yaml'),
                       'textureAddressingSha256':digest(dump/'texture_addressing.json'),
                       'mtlSha256':digest(obj.with_suffix('.mtl')),
                       'mesh':entry['mesh'],'triangles':{k:v['triangles'] for k,v in stats.items()}})
        if index%20==0:print(f'Rebuilt {index+1}/{len(jobs)} meshes',flush=True)
    (a.output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    provenance={'sourceManifestSha256':digest(a.pack/'manifest.json'),
                'importerSha256':digest(Path(__file__).with_name('gt4_mesh.py')),
                'profilesSha256':digest(a.profiles),'cars':len(manifest['cars']),
                'meshes':len(jobs),'models':report,'headsetRuntimeVerified':False}
    if missing:provenance['missingDumpToolSha256']=digest(a.tool)
    (a.output/'provenance.json').write_text(json.dumps(provenance,indent=2)+'\n')
    print(f'Ready: {len(manifest["cars"])} mappings, {len(jobs)} meshes',flush=True)


if __name__=='__main__':main()
