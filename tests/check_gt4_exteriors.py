"""Owned-data exterior corpus: colour inner surfaces and preserve authored geometry.

Requires the native gt2gt4visual target. Captures stay under work. Green pixels
are review markers, not an automatic hole verdict: open roofs and authored
window openings can legitimately show the inside.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--disc',type=Path,required=True)
    p.add_argument('--pack',type=Path,required=True)
    p.add_argument('--expansion',type=Path,required=True)
    p.add_argument('--exe',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--jobs',type=int,default=4)
    a=p.parse_args()
    root=Path(__file__).resolve().parents[1]
    output=a.output.resolve()
    if not output.is_relative_to(root/'work') or output.exists():p.error('Choose a new directory under work')
    if not 1<=a.jobs<=4:p.error('jobs must be 1..4')
    manifest=a.pack.resolve()/'manifest.json'
    manifest_before=hashlib.sha256(manifest.read_bytes()).hexdigest()
    cars=json.loads(manifest.read_text())['cars']
    ids=list(cars)
    output.mkdir(parents=True)
    env={**os.environ,'GT2_AUDIT_PREVIEW':'1','GT2_DEBUG_EXTERIOR':'1',
         'GT2_AUDIT_REQUIRE_INTACT':'1','GT2_CAR_EXPANSION':str(a.expansion.resolve())}
    env.pop('GT2_AUDIT_GENERAL_MODEL_SHADER',None)
    def batch(index):
        dest=output/f'batch-{index}'
        with (output/f'batch-{index}.log').open('w',encoding='utf8') as log:
            run=subprocess.run([str(a.exe.resolve()),str(a.disc.resolve()),str(a.pack.resolve()),str(dest),
                                *ids[index::a.jobs]],cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT)
        if run.returncode:raise RuntimeError(f'Batch {index} failed: see {log.name}')
        rows=json.loads((dest/'report.json').read_text())
        for row in rows:
            row['captureDirectory']=str(dest/'frames')
            if not row.get('captureComplete') or len(row.get('images',[]))!=8:raise RuntimeError(str(row))
            if any(not (dest/'frames'/f).is_file() for f in row['images']):raise RuntimeError('Missing image: '+row['id'])
        print(f'Batch {index}: {len(rows)} cars passed',flush=True)
        return rows
    rows=[]
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        for result in pool.map(batch,range(a.jobs)):rows.extend(result)
    assert len(rows)==len(cars) and {r['id'] for r in rows}==set(cars)
    assert hashlib.sha256(manifest.read_bytes()).hexdigest()==manifest_before
    rows.sort(key=lambda r:ids.index(r['id']))
    report={'cars':len(rows),'meshes':len({e['mesh'] for e in cars.values()}),
            'exteriorCaptures':len(rows)*8,'geometryPreserved':all(
                r['authoredExteriorVertices']==r['exteriorVertices'] and abs(r['removedExteriorSquareMetres'])<1e-6 for r in rows),
            'manifestSha256':manifest_before,'greenPixelsAreReviewMarkers':True,'headsetAccepted':False,'models':rows}
    (output/'report.json').write_text(json.dumps(report,indent=2),encoding='utf8')
    print(f'{len(rows)} cars / {report["meshes"]} meshes / {len(rows)*8} angles passed',flush=True)


if __name__=='__main__':main()
