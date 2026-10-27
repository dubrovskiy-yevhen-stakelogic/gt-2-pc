"""Audit native showroom reflection coverage and wheel placement for every mapping."""
import argparse, concurrent.futures, csv, hashlib, json, os, shutil, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for key in ('disc','pack','native','expansion','output'):p.add_argument('--'+key,type=Path,required=True)
    p.add_argument('--build',type=Path,default=ROOT/'build_gt4_models')
    p.add_argument('--workers',type=int,default=4)
    a=p.parse_args();out=a.output.absolute();out.relative_to(ROOT/'work');out.mkdir(exist_ok=False)
    exe=out/'gt2showroomaudit.exe';shutil.copy2(a.build/'gt2showroomaudit.exe',exe)
    ids=sorted(json.loads((a.native/'manifest.json').read_text())['cars'])
    if 't2h8r' not in ids:raise ValueError('Missing GT-One regression witness')
    env={k:v for k,v in os.environ.items() if not k.startswith('GT2_')}
    env.update(GT2_GT4_NATIVE=str(a.native.absolute()),GT2_CAR_EXPANSION=str(a.expansion.absolute()))
    def run(label,cars,legacy=False):
        folder=out/label;folder.mkdir();local=dict(env)
        if legacy:local['GT2_AUDIT_LEGACY_REFLECTION_COVERAGE']='1'
        with (folder/'run.log').open('w') as log:
            result=subprocess.run([str(exe),str(a.disc.absolute()),str(a.pack.absolute()),str(folder),*cars],cwd=ROOT,env=local,stdout=log,stderr=subprocess.STDOUT,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        return label,result.returncode
    _,negative=run('legacy',['t2h8r'],True)
    if negative!=1 or 'reflections fill' not in (out/'legacy/run.log').read_text():raise ValueError('GT-One film was not reproduced')
    batches=[ids[i::a.workers] for i in range(a.workers)]
    with concurrent.futures.ThreadPoolExecutor(a.workers) as pool:
        results=dict(pool.map(lambda job:run(*job),[(f'batch-{i}',cars) for i,cars in enumerate(batches)]))
    rows=[]
    for label in results:rows.extend(csv.DictReader((out/label/'coverage.csv').open()))
    if len(rows)!=len(ids)*3 or {row['car'] for row in rows}!=set(ids):raise ValueError('Incomplete catalogue coverage')
    witness=[row for row in rows if row['car']=='t2h8r']
    if sum(int(row['reflected_pixels']) for row in witness)<1000:raise ValueError('Reflections were disabled instead of masked')
    failures=[row for row in rows if int(row['unsupported_pixels'])>8]
    report=dict(status='failed' if any(results.values()) else 'passed',cars=len(ids),views=len(rows),
                legacy_failed=True,exit_codes=results,failures=failures,
                executable_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                wheels_sha256=hashlib.sha256((a.native/'wheels.json').read_bytes()).hexdigest())
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report),flush=True);return int(any(results.values()))

if __name__=='__main__':raise SystemExit(main())
