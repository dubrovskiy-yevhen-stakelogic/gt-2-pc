"""Exercise the standard camera cycle and saved selection in isolated real races."""
import argparse,json,os,re,subprocess
from pathlib import Path

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('game','disc','output'):p.add_argument('--'+name,type=Path,required=True)
 a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
 settings=out/'settings.txt';settings.write_text('frame_rate=original\nframe_cap=60\nvsync=0\n')
 overlay=Path(str(settings)+'.overlay');overlay.write_text('hd_assets=0\ngt4_model_set=0\n')
 (out/'wheel-settings.txt').write_text('version 2\noptions 0 0 0 25 10 0\nauto 0 0 0 "" ""\n')
 cases=[('cycle','20:c,40:c,60:c',[0,1,2,3],3),('reload-hidden','',[3],3),
        ('wrap','20:c',[3,0],0),('select-far','20:c,40:c',[0,1,2],2),
        ('retry-far','20:backspace',[2],2),('reload-far','',[2],2)]
 report=[]
 for name,script,expected,saved in cases:
  cmd=[str(a.game.resolve()),str(a.disc.resolve()),'--race','--car','t2vzn','--cars','1','--no-countdown','--no-sound','--no-music',
       '--settings',str(settings),'--card',str(out/'card.mcd'),'--fake-pad','0:type=digital','--shot','90',str(out/(name+'.png'))]
  if script:cmd+=['--script',script]
  if name=='cycle':
   for frame,label in [(10,'cockpit'),(30,'near'),(50,'far')]:cmd+=['--shot-at',str(frame),str(out/(label+'.png'))]
  with (out/(name+'.log')).open('w') as log:
   subprocess.run(cmd,env=dict(os.environ,GT2_WINDOW='hidden',GT2_NO_FOCUS='1'),stdout=log,stderr=subprocess.STDOUT,check=True,timeout=90,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
  text=(out/(name+'.log')).read_text();views=[int(x) for x in re.findall(r'camera: selected view (\d)',text)]
  assert views==expected,(name,views,expected)
  values=dict(x.split('=',1) for x in overlay.read_text().splitlines() if '=' in x)
  assert values.get('race_view')==str(saved),(name,values)
  assert 'cockpit_hidden_car' not in values
  if name=='retry-far':
   positions=re.findall(r'camera: position (\d)',text)
   assert positions and all(x=='2' for x in positions),(name,positions)
  report.append(dict(case=name,views=views,saved=saved))
 (out/'report.json').write_text(json.dumps(report,indent=2))
 print('PASS: four-view cycle, hidden view reload, wrap, chase reload and retry; no separate hidden-view setting')
if __name__=='__main__':main()
