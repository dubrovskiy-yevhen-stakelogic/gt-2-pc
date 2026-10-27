#!/usr/bin/env python3
"""Prove depth-writing lamp lenses cannot cut cockpit body panels."""
from pathlib import Path
import argparse, hashlib, json, os, shutil, subprocess

ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('disc','pack','native','expansion','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--exe',type=Path,default=ROOT/'build_gt4_models/gt2surfaceaudit.exe')
    parser.add_argument('--window-exe',type=Path,default=ROOT/'build_gt4_models/gt2windowaudit.exe')
    args=parser.parse_args()
    out=Path(os.path.abspath(args.output));out.relative_to(ROOT/'work')
    if out==ROOT/'work':parser.error('Use a fresh directory under work')
    out.mkdir(exist_ok=False)
    exe=out/'gt2surfaceaudit.exe';shutil.copy2(args.exe,exe)
    window_exe=out/'gt2windowaudit.exe';shutil.copy2(args.window_exe,window_exe)
    env={k:v for k,v in os.environ.items() if not k.startswith('GT2_')}
    env.update(GT2_WINDOW='hidden',GT2_NO_FOCUS='1',GT2_AUDIT_COCKPIT='1',GT2_AUDIT_WHEEL_COLOR='1',
        GT2_GT4_NATIVE=str(args.native.absolute()),GT2_GT4_MODEL_SET=str(args.pack.absolute()),
        GT2_CAR_EXPANSION=str(args.expansion.absolute()))
    report={'passed':False,'exe_sha256':hashlib.sha256(exe.read_bytes()).hexdigest()}
    for name,negative in [('before',True),('after',False)]:
        dest=out/name;dest.mkdir()
        run_env=dict(env)
        if negative:run_env['GT2_AUDIT_UNBOUNDED_DEPTH_GLASS']='1'
        with (dest/'run.log').open('w') as log:
            result=subprocess.run([str(exe),str(args.disc.absolute()),str(args.pack.absolute()),str(dest),'t2prn','z0090'],
                cwd=ROOT,env=run_env,stdout=log,stderr=subprocess.STDOUT,
                creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        gate=json.loads((dest/'gate.json').read_text());report[name]=dict(exit_code=result.returncode,**gate)
        if gate['paths']!=3 or gate['mirror_leaks']:
            raise RuntimeError('Missing model coverage or mirror regression')
        if negative:
            if result.returncode!=1 or gate['failures']!=6:raise RuntimeError('Prior lamp cuts not reproduced')
        elif result.returncode or gate['failures']:raise RuntimeError('Lamp/body cuts remain')
    # A longitudinal-only bound repairs lamps but cuts a low racing screen.
    # Require the actual GT40 screen to stay clear above the cowl as well.
    for name,negative in [('flat-screen-boundary',True),('preserved-screen',False)]:
        dest=out/name;dest.mkdir();run_env=dict(env)
        if negative:run_env['GT2_AUDIT_FLAT_DEPTH_GLASS_BOUNDARY']='1'
        with (dest/'run.log').open('w') as log:
            result=subprocess.run([str(window_exe),str(args.disc.absolute()),str(args.pack.absolute()),str(dest),'uf4lr','z0316'],
                cwd=ROOT,env=run_env,stdout=log,stderr=subprocess.STDOUT,
                creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        gate=json.loads((dest/'gate.json').read_text());report[name]=dict(exit_code=result.returncode,**gate)
        if gate['paths']!=3 or gate['missing_models']:raise RuntimeError('Missing GT40 coverage')
        if negative:
            if result.returncode!=1 or not gate['failures']:raise RuntimeError('Flat boundary did not reproduce tinted screen')
        elif result.returncode or gate['failures']:raise RuntimeError('Low racing screen is obstructed')
    report['window_exe_sha256']=hashlib.sha256(window_exe.read_bytes()).hexdigest()
    report['passed']=True
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))

if __name__=='__main__':main()
