"""Exercise per-car/model height persistence."""
import argparse,os,subprocess
from pathlib import Path

def main():
 p=argparse.ArgumentParser();p.add_argument('--game',type=Path,required=True);p.add_argument('--disc',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
 out=a.output.resolve();out.mkdir(parents=True,exist_ok=False);s=out/'settings.txt';s.write_text('frame_rate=0\nframe_cap=60\n');overlay=s.with_suffix('.txt.overlay')
 values={'hd_assets':'0','cockpit':'1','gt4_model_set':'1'}
 expected={}
 cases=[('gt4-seat','t2vzn',1,['down']*8+['right'],{'cockpit_car_height_gt4_t2vzn':'2'}),
        ('gt2-seat','t2vzn',0,['down']*8+['right','right'],{'cockpit_car_height_gt2_t2vzn':'4'}),
        ('other-car','h2s2n',0,['down']*8+['left'],{'cockpit_car_height_gt2_h2s2n':'-2'}),
        ('reload-gt4','t2vzn',1,['down']*8,{}),
        ('reset-gt2','t2vzn',0,['down']*9+['enter'],{'cockpit_car_height_gt2_t2vzn':None})]
 for name,car,gt4,keys,changes in cases:
  values['gt4_model_set']=str(gt4);overlay.write_text(''.join(f'{k}={v}\n' for k,v in values.items()))
  script='1:f10,3:up,5:up,7:up,9:up,11:enter'
  for i,key in enumerate(keys):script+=f',{15+i*2}:{key}'
  frame=20+len(keys)*2;script+=f',{frame+5}:esc,{frame+7}:esc'
  cmd=[str(a.game.resolve()),str(a.disc.resolve()),'--race','--car',car,'--cars','1','--no-countdown','--camera','0','--no-sound','--no-music','--settings',str(s),'--card',str(out/'card.mcd'),'--fake-pad','0:type=digital','--script',script,'--shot-at',str(frame),str(out/(name+'-menu.png')),'--shot',str(frame+35),str(out/(name+'-race.png'))]
  with (out/(name+'.log')).open('w') as log:subprocess.run(cmd,env=dict(os.environ,GT2_NO_FOCUS='1',GT2_WINDOW='hidden'),stdout=log,stderr=subprocess.STDOUT,check=True,timeout=90,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
  values=dict(line.split('=',1) for line in overlay.read_text().splitlines() if '=' in line and not line.startswith('#'))
  expected.update(changes)
  assert values.get('cockpit_seat_height')=='0',(name,'global height changed')
  for key,value in expected.items():assert values.get(key)==value,(name,key,value,values.get(key))
 print('PASS: independent GT2/GT4/car heights, reload, individual reset')
if __name__=='__main__':main()
