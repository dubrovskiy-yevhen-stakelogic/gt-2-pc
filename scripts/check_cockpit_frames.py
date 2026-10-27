"""Preserve authored upper frames and reject exterior inner-shell backfaces."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with Path(path).open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('disc', 'pack', 'native', 'expansion', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--build', type=Path, default=ROOT/'build_gt4_models')
    parser.add_argument('--workers', type=int, choices=range(1,5), default=4)
    args = parser.parse_args()
    out = Path(os.path.abspath(args.output))
    out.relative_to(ROOT/'work')
    if out == ROOT/'work':
        parser.error('Use a fresh directory under work')
    out.mkdir(exist_ok=False)
    for name in ('gt2frameaudit', 'gt2windowaudit'):
        shutil.copy2(args.build/(name+'.exe'), out/(name+'.exe'))
    env = {k:v for k,v in os.environ.items() if not k.startswith('GT2_AUDIT_') and k not in ('GT2_DEBUG_CABIN','GT2_DEBUG_EXTERIOR')}
    env.update(GT2_WINDOW='hidden', GT2_NO_FOCUS='1', GT2_AUDIT_COCKPIT='1',
        GT2_GT4_NATIVE=str(args.native.absolute()), GT2_CAR_EXPANSION=str(args.expansion.absolute()),
        GT2_GT4_MODEL_SET=str(args.pack.absolute()))
    report = dict(status='running', exe_sha256=digest(out/'gt2frameaudit.exe'),
        disc_sha256=digest(args.disc), pack_sha256=digest(args.pack/'manifest.json'),
        native_sha256=digest(args.native/'manifest.json'))

    def save():
        (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')

    def run(dest, ids, extra=None, name='gt2frameaudit'):
        dest.mkdir(exist_ok=True)
        with (dest/'render.log').open('w') as log:
            result = subprocess.run([str(out/(name+'.exe')),str(args.disc.absolute()),str(args.pack.absolute()),str(dest),*ids],
                cwd=ROOT,env=dict(env,**(extra or {})),stdout=log,stderr=subprocess.STDOUT,
                creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        return result.returncode

    save()
    try:
        inventory = out/'inventory'
        if run(inventory,['--list'],name='gt2windowaudit'):
            raise RuntimeError('Model inventory failed')
        expected = {(x['car'],x['path']) for x in csv.DictReader((inventory/'catalogue.csv').open())}
        if not expected or ('vst6n','gt4') not in expected:
            raise RuntimeError('Empty catalogue or missing Tuscan negative control')
        negative = out/'negative-cut-frame'
        code = run(negative,['vst6n'],{'GT2_AUDIT_CUT_UPPER_SHELL':'1'})
        witness = json.loads((negative/'gate.json').read_text())
        if code != 1 or witness['paths'] != 2 or not witness['failures']:
            raise RuntimeError('Old upper-frame cutting did not fail')
        structure_control=out/'negative-stock-structure'
        code=run(structure_control,['vst6n'],{'GT2_AUDIT_REMOVE_STRUCTURE':'1'})
        stock=next(x for x in csv.DictReader((structure_control/'frames.csv').open()) if x['path']=='gt2')
        if code!=1 or int(stock['pillar_missing'])==0 or int(stock['mount_missing'])==0:
            raise RuntimeError('Stock broken pillar and floating mirror controls did not both fail')
        report['stock_structure_negative_control']=stock
        # Rear-window borders in the reported Tuscan view, both stereo eyes.
        # These fixed regions exclude the real aerial, tyres and window centre.
        def black_edges(folder):
            im=Image.open(folder/'vst6n-gt4-4.png').convert('RGB')
            if im.size != (512,1024):raise RuntimeError('Unexpected exterior capture dimensions')
            return [sum(max(p)<16 for p in im.crop((x0,y0+eye*512,x1,y1+eye*512)).getdata())
                for eye in range(2) for x0,y0,x1,y1 in [(138,275,180,340),(330,275,378,340)]]
        before=out/'negative-backfaces';after=out/'corrected-backfaces'
        for folder,extra in [(before,{'GT2_AUDIT_RETAIN_NATIVE_BACKFACES':'1','GT2_AUDIT_CAPTURE':'1'}),
                             (after,{'GT2_AUDIT_CAPTURE':'1'})]:
            if run(folder,['vst6n'],extra):raise RuntimeError('Exterior control capture failed')
        bad,good=black_edges(before),black_edges(after)
        if sum(bad)<100 or max(good)>8:raise RuntimeError(f'Tuscan inner-shell leak: before={bad}, after={good}')
        report.update(negative_control=witness,exterior_backface_control={'before_pixels':bad,'after_pixels':good},expected_paths=len(expected));save()
        ids = sorted({x[0] for x in expected})
        with ThreadPoolExecutor(max_workers=args.workers) as pool:
            codes = list(pool.map(lambda i:run(out/f'shard-{i}',ids[i::args.workers]),range(args.workers)))
        rows=[];gates=[]
        for i in range(args.workers):
            folder=out/f'shard-{i}'
            rows.extend(csv.DictReader((folder/'frames.csv').open()))
            gates.append(json.loads((folder/'gate.json').read_text()))
        if {(x['car'],x['path']) for x in rows} != expected or len(rows) != len(expected):
            raise RuntimeError('Incomplete or duplicate model paths')
        if any(g['backface_negative_pixels']<100 or g['backface_corrected_pixels']>8 for g in gates):
            raise RuntimeError('Backface raster negative/corrected controls failed')
        with (out/'frames.csv').open('w',newline='') as target:
            writer=csv.DictWriter(target,fieldnames=rows[0]);writer.writeheader();writer.writerows(rows)
        report.update(paths=len(rows),failures=sum(g['failures'] for g in gates),
            max_vertices=max(int(x['vertices']) for x in rows),exit_codes=codes)
        if any(codes) or report['failures']:
            raise RuntimeError('Upper-frame preservation failed')
        report['status']='passed'
    except Exception as error:
        report.update(status='failed',error=str(error))
        raise
    finally:
        save()


if __name__ == '__main__':
    main()
