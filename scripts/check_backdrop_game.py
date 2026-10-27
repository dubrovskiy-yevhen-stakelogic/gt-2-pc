"""Check the production race renderer's backdrop ordering with an owned disc."""
from pathlib import Path
import argparse, json, os, subprocess
from check_backdrop_regression import digest

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--disc', required=True, type=Path)
    p.add_argument('--game', type=Path, default=ROOT/'build_gt4_models/gt2game.exe')
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--course', default='autumn')
    args = p.parse_args()
    out = args.output.resolve()
    if not out.is_relative_to(ROOT/'work') or out == ROOT/'work':
        p.error('output must be a new directory under work/')
    out.mkdir(parents=True, exist_ok=False)
    report = dict(game_sha256=digest(args.game), course=args.course, cases=[], status='running')
    env = dict(os.environ, GT2_WINDOW='hidden', GT2_NO_FOCUS='1', GT2_AUDIT_BACKDROP_ORDER='1')
    for key in ('GT2_RENDER_CAPTURE', 'GT2_AUDIT_SKY_FIRST'):
        env.pop(key, None)
    for camera, negative in ((2, True), (0, False), (2, False), (3, False)):
        dest = out/('negative' if negative else 'camera-'+str(camera));dest.mkdir()
        (dest/'settings.txt').write_text('draw_distance=all\ntexture_filter=smooth\nmsaa=2\n')
        runenv = dict(env)
        if negative:
            runenv['GT2_AUDIT_SKY_FIRST'] = '1'
        cmd = [str(args.game.resolve()), str(args.disc.resolve()), '--race', '--track', args.course,
               '--car', 'h2s2n', '--cars', '1', '--ai-player', '--no-countdown', '--camera', str(camera),
               '--no-sound', '--no-music', '--settings', str(dest/'settings.txt'),
               '--card', str(dest/'slot1.mcd'), '--card2', str(dest/'slot2.mcd'),
               '--fast', '--window', '640x480', '--shot', '120', str(dest/'frame.png')]
        run = subprocess.run(cmd, cwd=ROOT, env=runenv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             timeout=180, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        log = run.stdout.decode('utf-8', 'replace');(dest/'run.log').write_text(log)
        marker = 'backdrop regression: opaque world submitted after sky' in log
        passed = (run.returncode != 0 and marker) if negative else (run.returncode == 0 and (dest/'frame.png').is_file() and not marker)
        report['cases'].append(dict(camera=camera, negative=negative, exit_code=run.returncode, passed=passed))
        (out/'report.json').write_text(json.dumps(report, indent=2))
    report['status'] = 'passed' if all(x['passed'] for x in report['cases']) else 'failed'
    (out/'report.json').write_text(json.dumps(report, indent=2));print(json.dumps(report))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
