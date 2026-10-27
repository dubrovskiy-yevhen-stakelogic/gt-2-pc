"""Local render integration checks against installed GT2 and a private GT4 pack."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--game',required=True,type=Path);p.add_argument('--disc',required=True,type=Path)
    p.add_argument('--pack',required=True,type=Path);p.add_argument('--output',required=True,type=Path)
    p.add_argument('--cars',nargs='+',help='Representative model checks; default is every manifest entry')
    args=p.parse_args(); out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    report={}
    def run(name,enabled,pack=None,car='dvpgn',camera=0,script=None,drive=False):
        folder=out/name;folder.mkdir();settings=folder/'settings.txt'
        settings.write_text('frame_rate=original\nframe_cap=60\ntexture_filter=smooth\nmsaa=4\n')
        overlay=settings.with_suffix('.txt.overlay');overlay.write_text(f'gt4_model_set={enabled}\ncockpit=1\n')
        command=[str(args.game.resolve()),str(args.disc.resolve()),'--race','--track','seattle','--car',car,
                 '--cars','1','--seed','1','--no-countdown','--camera',str(camera),'--no-sound','--no-music','--no-hud',
                 '--settings',str(settings),'--card',str(folder/'card.mcd'),'--card2',str(folder/'card2.mcd'),
                 '--fake-pad','0:type=digital','--shot','120',str(folder/'frame.png')]
        if script: command += ['--script',script,'--shot-at','20',str(folder/'menu.png')]
        if drive: command += ['--drive']
        env=dict(os.environ,GT2_NO_FOCUS='1',GT2_WINDOW='hidden',GT2_GT4_MODEL_SET=str((pack or args.pack).resolve()))
        log=subprocess.run(command,env=env,capture_output=True,text=True,timeout=60)
        (folder/'run.log').write_text(log.stdout+log.stderr,encoding='utf8')
        if log.returncode: raise RuntimeError(name+': game failed')
        frame=folder/'frame.png'; assert frame.stat().st_size>10000
        telemetry=re.findall(r'^step\s+.*$',log.stdout,re.M);assert telemetry
        result={'sha256':hashlib.sha256(frame.read_bytes()).hexdigest(),'telemetry':telemetry[-1],
                'models':re.findall(r'^GT4 model set:.*$',log.stdout,re.M),
                'enabled':dict(line.split('=',1) for line in overlay.read_text().splitlines() if '=' in line)['gt4_model_set']}
        report[name]=result; return result
    off=run('off',0,drive=True)
    on=run('on',1,drive=True)
    missing=run('missing',1,pack=out/'no-pack',drive=True)
    malformed=out/'malformed-pack';malformed.mkdir();(malformed/'manifest.json').write_text('{broken')
    bad=run('malformed',1,pack=malformed,drive=True)
    assert off['sha256']==missing['sha256']==bad['sha256'], 'Fallback image differs from OFF'
    assert on['sha256']!=off['sha256'], 'ON did not change the mesh'
    assert off['telemetry']==on['telemetry']==missing['telemetry']==bad['telemetry'], 'Simulation changed'
    assert any('loaded' in line for line in on['models'])
    cars=json.loads((args.pack/'manifest.json').read_text())['cars']
    for car in args.cars or cars:
        assert car in cars, 'Requested car absent from pack: '+car
        if car=='dvpgn': continue
        assert any('loaded' in line for line in run(car,1,car=car)['models'])
    run('exterior',1,camera=1)
    unsupportedOff=run('unsupported-off',0,car='us36n')
    unsupportedOn=run('unsupported-on',1,car='us36n')
    assert unsupportedOff['sha256']==unsupportedOn['sha256'], 'Unmapped car changed'
    toggle='5:f10,7:up,9:up,11:up,13:up,15:enter,17:up,19:up,21:right,25:esc,27:esc'
    assert run('toggle-on',0,script=toggle)['enabled']=='1'
    assert run('toggle-off',1,script=toggle)['enabled']=='0'
    (out/'report.json').write_text(json.dumps(report,indent=2))
    print(f'PASS: {len(args.cars or cars)} selected cars; ON/OFF; exact missing/malformed/unmapped fallback; matching race telemetry; live menu toggles saved.')


if __name__=='__main__':main()
