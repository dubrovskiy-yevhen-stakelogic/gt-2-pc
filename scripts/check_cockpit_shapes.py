"""Check generated fascia shape, with a watertight two-hump negative control."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
from concurrent.futures import ThreadPoolExecutor

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('disc', 'pack', 'native', 'expansion', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--build', type=Path, default=ROOT/'build_gt4_models')
    parser.add_argument('--workers', type=int, choices=range(1,5), default=2)
    args = parser.parse_args()
    out = Path(os.path.abspath(args.output))
    out.relative_to(ROOT/'work')
    if out == ROOT/'work':
        parser.error('Use a fresh directory under work')
    out.mkdir(exist_ok=False)
    for name in ('gt2shapeaudit', 'gt2windowaudit'):
        shutil.copy2(args.build/(name+'.exe'), out/(name+'.exe'))
    env = {k:v for k,v in os.environ.items() if not k.startswith('GT2_')}
    env.update(GT2_WINDOW='hidden', GT2_NO_FOCUS='1', GT2_AUDIT_COCKPIT='1',
        GT2_GT4_NATIVE=str(args.native.absolute()), GT2_CAR_EXPANSION=str(args.expansion.absolute()),
        GT2_GT4_MODEL_SET=str(args.pack.absolute()))

    def digest(path):
        with path.open('rb') as stream:
            return hashlib.file_digest(stream, 'sha256').hexdigest()

    report = dict(status='running', exe_sha256=digest(out/'gt2shapeaudit.exe'),
        disc_sha256=digest(args.disc), pack_sha256=digest(args.pack/'manifest.json'),
        native_sha256=digest(args.native/'manifest.json'))

    def run(folder, ids, executable='gt2shapeaudit', capture=False, legacy=False):
        folder.mkdir(exist_ok=True)
        local = dict(env)
        if capture:
            local['GT2_AUDIT_CAPTURE']='1'
        if legacy:
            local['GT2_AUDIT_LEGACY_COWL']='1'
        with (folder/'render.log').open('w') as log:
            return subprocess.run([str(out/(executable+'.exe')),str(args.disc.absolute()),
                str(args.pack.absolute()),str(folder),*ids],cwd=ROOT,env=local,stdout=log,
                stderr=subprocess.STDOUT,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0)).returncode

    try:
        inventory=out/'inventory'
        if run(inventory,['--list'],'gt2windowaudit'):
            raise RuntimeError('Catalogue inventory failed')
        expected={(r['car'],r['path']) for r in csv.DictReader((inventory/'catalogue.csv').open())}
        if ('vst6n','gt2') not in expected:
            raise RuntimeError('Reported TVR is absent')
        report['expected_paths']=len(expected)
        negative=out/'tvr-legacy'
        if run(negative,['vst6n'],capture=True,legacy=True)!=1:
            raise RuntimeError('The original TVR two-hump deformation did not fail')
        witness=json.loads((negative/'gate.json').read_text())
        rows=list(csv.DictReader((negative/'shape.csv').open()))
        if witness['failures']!=1 or not any(r['path']=='gt2' and r['bad']=='1' for r in rows):
            raise RuntimeError('Wrong original-deformation witness')
        report['legacy_negative_control']=witness
        # Retain both-side/downward images of the reported car for visual review.
        if run(out/'tvr',['vst6n'],capture=True):
            raise RuntimeError('Reported TVR shape failed')
        ids=sorted({key[0] for key in expected})
        with ThreadPoolExecutor(max_workers=args.workers) as pool:
            codes=list(pool.map(lambda i:run(out/f'shard-{i}',ids[i::args.workers]),range(args.workers)))
        rows=[];gates=[]
        for i in range(args.workers):
            folder=out/f'shard-{i}'
            rows.extend(csv.DictReader((folder/'shape.csv').open()))
            gates.append(json.loads((folder/'gate.json').read_text()))
        if {(r['car'],r['path']) for r in rows}!=expected or len(rows)!=len(expected):
            raise RuntimeError('Missing or duplicate model paths')
        with (out/'shape.csv').open('w',newline='') as stream:
            writer=csv.DictWriter(stream,fieldnames=rows[0]);writer.writeheader();writer.writerows(rows)
        report.update(paths=len(rows),poses=sum(g['poses'] for g in gates),
            failures=sum(g['failures'] for g in gates),exit_codes=codes,
            deformation_controls=all(g['deformation_control'] for g in gates))
        if any(codes) or report['failures'] or not report['deformation_controls']:
            raise RuntimeError('Generated fascia shape regression')
        report['status']='passed'
    except Exception as error:
        report.update(status='failed',error=str(error))
        raise
    finally:
        (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')


if __name__=='__main__':
    main()
