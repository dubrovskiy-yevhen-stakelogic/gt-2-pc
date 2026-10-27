"""Run GT2's offscreen corpus plus owned PCSX2 reference cases, then compare.

All outputs are private, below work. Config paths must be absolute. References
use a checked car name/ID, a local state hash and recorded menu/capture steps.
The original emulator configuration, ISO and user cards are never modified.
"""
import argparse,hashlib,json,os,subprocess,sys
from pathlib import Path


def digest(path):
    with Path(path).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()


def run(command,log,env=None):
    options={}
    if os.name=='nt':
        startup=subprocess.STARTUPINFO();startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow=subprocess.SW_HIDE
        options=dict(startupinfo=startup,creationflags=subprocess.CREATE_NO_WINDOW)
    with log.open('w',encoding='utf-8') as f:
        subprocess.run([str(x) for x in command],stdout=f,stderr=subprocess.STDOUT,
                       env=env,check=True,**options)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--config',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--cars',nargs='+',help='Optional subset of imported IDs; default all mappings')
    p.add_argument('--workers',type=int,choices=range(1,5),default=4,
                   help='Independent offscreen workers for a complete catalogue')
    a=p.parse_args();scripts=Path(__file__).resolve().parent;work=scripts.parent/'work';out=a.output.resolve()
    if not out.is_relative_to(work) or out.exists():p.error('Choose a fresh output under work')
    cfg=json.loads(a.config.read_text());out.mkdir()
    for key in ('auditExecutable','gt2Disc','pack','expansion','emulator','gt4Iso','emulatorConfig'):
        if not Path(cfg[key]).is_absolute() or not Path(cfg[key]).exists():raise ValueError('Missing absolute path: '+key)
    env=dict(os.environ,GT2_CAR_EXPANSION=cfg['expansion'])
    if a.cars:
        run([cfg['auditExecutable'],cfg['gt2Disc'],cfg['pack'],out/'gt2',*a.cars],out/'gt2.log',env)
    else:
        run([sys.executable,scripts/'gt4_visual_corpus.py','--game',cfg['auditExecutable'],
             '--disc',cfg['gt2Disc'],'--pack',cfg['pack'],'--output',out/'gt2',
             '--workers',str(a.workers)],out/'gt2.log',env)
    refs=[];provenance=[]
    for case in cfg.get('references',[]):
        if a.cars and case['id'] not in a.cars:continue
        if not case.get('identityVerified'):raise ValueError('Check original GT4 car identity: '+case['id'])
        if digest(case['state'])!=case['stateSha256']:raise ValueError('Reference state changed: '+case['id'])
        target=out/('ps2-'+case['id'])
        command=[sys.executable,scripts/'gt4_pcsx2_reference.py','--emulator',cfg['emulator'],
                 '--iso',cfg['gt4Iso'],'--config',cfg['emulatorConfig'],'--output',target,
                 '--state',case['state'],'--steps',case['steps'],'--duration',str(case['duration'])]
        if case.get('card'):command+=['--card',case['card']]
        run(command,out/('ps2-'+case['id']+'.log'))
        report=json.loads((target/'report.json').read_text());provenance.append(report)
        refs.append(dict(id=case['id'],name=case['name'],identityVerified=True,
                         frames=[str(target/f['path']) for f in report['captures']],crop=case.get('crop')))
    (out/'references.json').write_text(json.dumps(refs,indent=2)+'\n')
    run([sys.executable,scripts/'gt4_compare_visuals.py','--audit',out/'gt2','--output',out/'comparison',
         '--references',out/'references.json'],out/'comparison.log')
    (out/'provenance.json').write_text(json.dumps(dict(configSha256=digest(a.config),
        auditExecutableSha256=digest(cfg['auditExecutable']),packManifestSha256=digest(Path(cfg['pack'])/'manifest.json'),
        gt4TocSha256=json.loads((scripts/'gt4_profiles.json').read_text())['discTocSha256'],
        referenceRuns=provenance,headsetAccepted=False),indent=2)+'\n')
    print(out/'comparison/index.html',flush=True)


if __name__=='__main__':main()
