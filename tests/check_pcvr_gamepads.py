"""Use the local OpenXR simulator plus a desktop pad to check the production merge."""
import argparse,csv,json,os,re,subprocess
from pathlib import Path

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('game','disc','runtime-json','output'):p.add_argument('--'+name,type=Path,required=True)
 a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
 pad='0:type=analog,0:lx=220,0:r2p=210'
 cases=[('flat',False,0,pad,'',''),('vr-stick',True,0,pad,'',''),('vr-wheel-idle',True,1,pad,'',''),
        ('both',True,1,'0:type=analog,0:lx=220','@0 right trigger 1\n@30 left y 1\n@40 left y 0\n',''),
        ('pc-camera',True,0,pad+',30:r1=1:10','',''),
        ('pc-custom',True,0,'0:type=analog,0:lx=220,0:cross=1','', 'pc_custom_bindings=1\npc_binding_0=3\n'),
        ('touch-only',True,0,'0:type=none','@0 right trigger 1\n@0 left thumbstick 0.7 0\n',''),
        ('release',True,1,pad+',90:lx=128,90:r2p=0','',''),
        ('hidden-stick',True,0,'0:type=none','','race_view=3\n'),
        ('hidden-wheel',True,1,'0:type=none','','race_view=3\n')]
 report={}
 for name,vr,mode,fake,actions,extra in cases:
  dest=out/name;dest.mkdir();s=dest/'settings.txt';s.write_text('frame_rate=original\nframe_cap=60\nvsync=0\nvr_multiview=1\n')
  overlay=Path(str(s)+'.overlay');overlay.write_text(f'hd_assets=0\ngt4_model_set=0\nvr_driving_mode={mode}\npc_brake_reverse=0\nvr_brake_reverse=0\n'+extra)
  (dest/'wheel-settings.txt').write_text('version 2\noptions 0 0 0 25 10 0\nauto 0 0 0 "" ""\n')
  env=dict(os.environ,GT2_WINDOW='hidden',GT2_NO_FOCUS='1')
  cmd=[str(a.game.resolve()),str(a.disc.resolve()),'--race','--car','t2vzn','--cars','1','--no-countdown','--no-sound','--no-music','--settings',str(s),'--card',str(dest/'card.mcd'),'--fake-pad',fake]
  if vr:
   script=dest/'xr.txt';script.write_text('resolution 480 512\ncapture 20 80\n@0 head pos 0 1.6 0 ypr 0 -12 0\n'+actions+'@200 event exit\n')
   env.update(XR_RUNTIME_JSON=str(a.runtime_json.resolve()),XRSIM_SCRIPT=str(script),XRSIM_OUT=str(dest),GT2_XR_MIRROR='0')
   cmd+=['--vr','--xr-deterministic']
  else:cmd+=['--shot','190',str(dest/'flat.png')]
  with (dest/'game.log').open('w') as log:subprocess.run(cmd,env=env,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
  log=(dest/'game.log').read_text()
  frames=[tuple(map(int,x)) for x in re.findall(r'pad: frame flags \w+ buttons \w+ steer axis (\d+) throttle (\d+) brake (\d+) -> steer (-?\d+) throttle (\d+) brake (\d+)',log)]
  assert frames,(name,'missing race input telemetry')
  if vr:
   rows=list(csv.DictReader((dest/'xrsim_frames.csv').open()));assert all(x['result']=='ok' for x in rows),(name,'XR submit failure')
   assert any(x['layers'].startswith('P2') for x in rows)
  report[name]=dict(frames=frames,cameras=re.findall(r'camera: selected view (\d)',log))
 base=report['flat']['frames'][0]
 assert base[0]>128 and base[1]>0,base
 for name in ['vr-stick','vr-wheel-idle']:assert report[name]['frames'][0]==base,(name,report[name],base)
 assert report['both']['frames'][0][0]==base[0] and report['both']['frames'][0][1]>base[1]
 for name in ['both','pc-camera']:assert report[name]['cameras']==['0','1'],(name,report[name])
 assert report['pc-custom']['frames'][0][1]>base[1]
 assert report['touch-only']['frames'][0][0]>128 and report['touch-only']['frames'][0][1]>0
 assert report['release']['frames'][-1][0]==128 and report['release']['frames'][-1][1]==0,report['release']
 from PIL import Image,ImageChops
 left=Image.open(out/'hidden-stick/f000020_l0_proj_v0.png').convert('RGB')
 right=Image.open(out/'hidden-wheel/f000020_l0_proj_v0.png').convert('RGB')
 diff=ImageChops.difference(left,right);changed=sum(any(p) for p in diff.getdata())
 assert changed>500,('virtual wheel absent in hidden view',changed)
 report['hidden-wheel-pixels']=changed
 (out/'report.json').write_text(json.dumps(report,indent=2))
 print('PASS: desktop input equals flat in PCVR; simultaneous Touch/gamepad; independent bindings and camera buttons; release; hidden-view wheel visible')
if __name__=='__main__':main()
