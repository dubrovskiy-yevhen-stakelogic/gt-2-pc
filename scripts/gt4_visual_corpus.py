"""Capture a private GT4 corpus using independent hidden offscreen workers."""
import argparse,json,os,shutil,subprocess
from pathlib import Path

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for key in ('game','disc','pack','output'):p.add_argument('--'+key,type=Path,required=True)
    p.add_argument('--workers',type=int,default=4)
    p.add_argument('--reuse-audit',type=Path,help='Reuse complete rows/frames from an interrupted private audit')
    a=p.parse_args();work=Path(__file__).resolve().parents[1]/'work';out=a.output.resolve()
    if not out.is_relative_to(work) or out.exists() or not 1<=a.workers<=4:p.error('Fresh work output; 1..4 workers required')
    expected=set(json.loads((a.pack/'manifest.json').read_text())['cars']);rows=[];sources={}
    if a.reuse_audit:
        seed=a.reuse_audit.resolve()
        if not seed.is_relative_to(work):p.error('Reuse only a private work audit')
        for row in json.loads((seed/'report.json').read_text()):
            files=row.get('images',[])+[row['id']+'-cockpit.png']
            if row['id'] in expected and row.get('captureComplete') and len(files)==9 and all((seed/'frames'/f).is_file() for f in files):
                rows.append(row);sources[row['id']]=seed/'frames'
    remaining=sorted(expected-set(sources));out.mkdir();jobs=[]
    options={}
    if os.name=='nt':
        startup=subprocess.STARTUPINFO();startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=subprocess.SW_HIDE
        options=dict(startupinfo=startup,creationflags=subprocess.CREATE_NO_WINDOW)
    for worker in range(a.workers):
        ids=remaining[worker::a.workers]
        if not ids:continue
        folder=out/('worker-'+str(worker));log=(out/('worker-'+str(worker)+'.log')).open('w')
        proc=subprocess.Popen([str(a.game.resolve()),str(a.disc.resolve()),str(a.pack.resolve()),str(folder),*ids],stdout=log,stderr=subprocess.STDOUT,**options)
        jobs.append((proc,log,folder))
    try:
        for proc,log,folder in jobs:
            code=proc.wait();log.close()
            if code:raise RuntimeError('Corpus worker failed: '+str(folder))
            for row in json.loads((folder/'report.json').read_text()):
                if not row.get('captureComplete') or row['id'] in sources:raise RuntimeError('Failed/duplicate car: '+row['id'])
                rows.append(row);sources[row['id']]=folder/'frames'
            print('Completed',folder.name,flush=True)
    finally:
        for proc,log,_ in jobs:
            if proc.poll() is None:proc.terminate();proc.wait(timeout=10)
            log.close()
    if set(sources)!=expected:raise RuntimeError('Incomplete corpus')
    (out/'frames').mkdir()
    for row in rows:
        for file in row['images']+[row['id']+'-cockpit.png']:
            source=sources[row['id']]/file;target=out/'frames'/file
            try:os.link(source,target)
            except OSError:shutil.copyfile(source,target)
    (out/'report.json').write_text(json.dumps(rows,indent=2)+'\n')
    (out/'capture-provenance.json').write_text(json.dumps(dict(workers=a.workers,reusedRows=len(expected)-len(remaining),
        captureComplete=True,headsetAccepted=False),indent=2)+'\n')
    print('Corpus complete:',len(rows),flush=True)

if __name__=='__main__':main()
