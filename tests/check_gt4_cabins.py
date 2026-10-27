"""Capture both eyes of every imported cabin for visual inspection.

Requires the local OpenXR simulator and privately extracted game/model data.
Success verifies capture completion, not absence of holes or headset performance.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('game', 'disc', 'pack', 'runtime', 'output'):
        p.add_argument('--' + name, required=True, type=Path)
    p.add_argument('--cars', nargs='+')
    p.add_argument('--debug', action='store_true', help='Green inner faces, magenta outer faces, blue wheels; verify cabin cache')
    p.add_argument('--keep-going', action='store_true', help='Report every failing car instead of stopping at the first')
    p.add_argument('--fast', action='store_true', help='Eight stationary poses at 720px with four settling frames each')
    args = p.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    cars = json.loads((args.pack / 'manifest.json').read_text())['cars']
    poses = [(0, 1.6, 0, 0, -20), (-.12, 1.6, -.10, 65, -25),
             (.12, 1.6, -.10, -65, -25), (0, 1.5, -.1, 0, -65),
             (0, 1.6, .12, 160, -30), (0, 1.6, .12, -160, -30),
             (0, 1.68, -.12, 0, 15), (0, 1.6, 0, 0, 0)]
    report = {}
    for car in args.cars or cars:
        if car not in cars:
            raise ValueError('Car absent from manifest: ' + car)
        folder = out / car
        folder.mkdir()
        settings = folder / 'settings.txt'
        settings.write_text('frame_rate=display\ntexture_filter=smooth\nmsaa=4\n')
        settings.with_suffix('.txt.overlay').write_text(
            'gt4_model_set=1\ncockpit=1\ncockpit_wheel=1\ncockpit_mirror=1\n')
        script = folder / 'input.txt'
        step = 4 if args.fast else 35
        first = 8 if args.fast else 30
        resolution = 720 if args.fast else 1100
        text = f'resolution {resolution} {resolution}\ncapture ' + ' '.join(str(first + i*step) for i in range(len(poses))) + '\n'
        for i, (x, y, z, yaw, pitch) in enumerate(poses):
            text += f'@{0 if i==0 else first+(i-1)*step+1} head pos {x} {y} {z} ypr {yaw} {pitch} 0\n'
        script.write_text(text + f'@{first+len(poses)*step+4} event exit\n')
        env = dict(os.environ, GT2_NO_FOCUS='1', GT2_WINDOW='hidden', GT2_XR_MIRROR='0',
                   GT2_GT4_MODEL_SET=str(args.pack.resolve()), XR_RUNTIME_JSON=str(args.runtime.resolve()),
                   XRSIM_SCRIPT=str(script), XRSIM_OUT=str(folder))
        env.pop('GT2_DEBUG_CABIN', None)
        if args.debug:
            env['GT2_DEBUG_CABIN'] = '1'
        command = [str(args.game.resolve()), str(args.disc.resolve()), '--race', '--track', 'seattle',
                   '--car', car, '--cars', '1', '--no-countdown', '--camera', '0', '--no-sound',
                   '--no-music', '--no-hud', '--settings', str(settings), '--card', str(folder/'card.mcd'),
                   '--card2', str(folder/'card2.mcd'), '--vr', '--xr-deterministic', '--drive']
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=90)
        log = result.stdout + result.stderr
        (folder/'run.log').write_text(log, encoding='utf8')
        images = sorted(f.name for f in folder.glob('*.png'))
        ok = result.returncode == 0 and f'GT4 model set: {car} loaded' in log and len(images) == 2*len(poses)
        builds=log.count('Cabin rebuild:')
        if args.debug: ok = ok and 1 <= builds <= 2
        report[car] = {'images': images, 'poses': poses, 'debug': args.debug, 'cabinBuilds': builds,
                       'passed': ok, 'exitCode': result.returncode}
        (out/'report.json').write_text(json.dumps(report, indent=2))
        print(car, 'captured both eyes at 8 poses' if ok else 'FAILED; see run.log', flush=True)
        if not ok and not args.keep_going: raise AssertionError((car,result.returncode,len(images)))
    (out/'report.json').write_text(json.dumps(report, indent=2))
    if not all(r['passed'] for r in report.values()): raise SystemExit(1)


if __name__ == '__main__':
    main()
