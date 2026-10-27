"""Full owned-asset cockpit-window gate, including a retained-glass negative control."""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
VIEWS = {(str(yaw), str(pitch)) for yaw in (0,-35,35,-70,70,-110,110) for pitch in (0,15)}


def digest(path):
    with Path(path).open('rb') as source:
        return hashlib.file_digest(source,'sha256').hexdigest()


def coverage(rows, expected, missing):
    seen = {}
    for row in rows:
        key = row['car'],row['path']
        seen.setdefault(key,Counter())[(row['yaw'],row['pitch'])] += 1
    if set(seen) | missing != expected or set(seen) & missing:
        raise RuntimeError('Missing, unexpected or partially rendered model paths')
    if any(set(poses) != VIEWS or any(n != 1 for n in poses.values()) for poses in seen.values()):
        raise RuntimeError('Missing or duplicate camera views')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for field in ('disc','pack','native','expansion','output'):
        parser.add_argument('--'+field,required=True,type=Path)
    parser.add_argument('--exe',type=Path,default=ROOT/'build_gt4_models/gt2windowaudit.exe')
    parser.add_argument('--workers',type=int,choices=range(1,5),default=4)
    parser.add_argument('--negative-car',default='vst6n')
    args=parser.parse_args()
    out=Path(os.path.abspath(args.output))
    out.relative_to(ROOT/'work')
    if out==ROOT/'work':parser.error('Use a new output directory under work')
    out.mkdir(parents=True,exist_ok=False)
    exe=out/'gt2windowaudit.exe';shutil.copy2(args.exe,exe)
    env=dict(os.environ,GT2_WINDOW='hidden',GT2_NO_FOCUS='1',GT2_AUDIT_COCKPIT='1',
             GT2_GT4_NATIVE=str(args.native.absolute()),GT2_CAR_EXPANSION=str(args.expansion.absolute()),
             GT2_GT4_MODEL_SET=str(args.pack.absolute()))
    for key in list(env):
        if key.startswith('GT2_AUDIT_') and key!='GT2_AUDIT_COCKPIT' or key in ('GT2_DEBUG_CABIN','GT2_DEBUG_EXTERIOR'):
            env.pop(key,None)
    report=dict(status='running',exe_sha256=digest(exe),disc_sha256=digest(args.disc),
                pack_sha256=digest(args.pack/'manifest.json'),native_sha256=digest(args.native/'manifest.json'))
    def save(): (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    def run(dest,ids,extra=None):
        dest.mkdir(exist_ok=True)
        with (dest/'render.log').open('w') as log:
            result=subprocess.run([str(exe),str(args.disc.absolute()),str(args.pack.absolute()),str(dest),*ids],
                cwd=ROOT,env=dict(env,**(extra or {})),stdout=log,stderr=subprocess.STDOUT,
                creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        return result.returncode
    save()
    try:
        inventory=out/'inventory'
        if run(inventory,['--list']):raise RuntimeError('Catalogue enumeration failed')
        expected={(x['car'],x['path']) for x in csv.DictReader((inventory/'catalogue.csv').open())}
        if not expected or (args.negative_car,'gt4') not in expected:
            raise RuntimeError('Empty catalogue or negative-control car absent')
        negative=out/'negative-retained-glass'
        code=run(negative,[args.negative_car],{'GT2_AUDIT_RETAIN_TINT':'1'})
        witness=json.loads((negative/'gate.json').read_text())
        if code!=1 or witness['missing_models'] or not witness['failures']:
            raise RuntimeError('Retained-glass negative control did not fail visibly')
        report['negative_control']=witness
        report['expected_paths']=len(expected)
        save()
        ids=sorted({x[0] for x in expected})
        with ThreadPoolExecutor(max_workers=args.workers) as pool:
            codes=list(pool.map(lambda i:run(out/f'shard-{i}',ids[i::args.workers]),range(args.workers)))
        rows=[];errors=[];gates=[]
        for i in range(args.workers):
            folder=out/f'shard-{i}'
            rows.extend(csv.DictReader((folder/'windows.csv').open()))
            errors.extend(csv.DictReader((folder/'errors.csv').open()))
            gates.append(json.loads((folder/'gate.json').read_text()))
        coverage(rows,expected,{(x['car'],x['path']) for x in errors})
        with (out/'windows.csv').open('w',newline='') as target:
            writer=csv.DictWriter(target,fieldnames=rows[0]);writer.writeheader();writer.writerows(rows)
        failures=sum(g['failures'] for g in gates)
        report.update(expected_paths=len(expected),rendered_paths=len(rows)//14,eye_views=len(rows)*2,
                      failures=failures,errors=errors,exit_codes=codes,csv_sha256=digest(out/'windows.csv'),
                      status='passed' if not any(codes) and not failures and not errors else 'failed')
        if digest(exe)!=report['exe_sha256']:raise RuntimeError('Audit snapshot changed')
    except BaseException as error:
        report.update(status='interrupted',error=str(error));save();raise
    save();print(json.dumps(report));return 0 if report['status']=='passed' else 1


if __name__=='__main__':
    raise SystemExit(main())
