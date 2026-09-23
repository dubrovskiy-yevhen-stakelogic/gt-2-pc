"""Capture the countdown camera cut with isolated cockpit on/off settings."""
import argparse
import csv
import json
import os
from pathlib import Path
import re
import subprocess

from PIL import Image, ImageChops, ImageStat

from check_cockpit import ROOT, inspect_png, prepare_case, require_file, sha256, write_gallery


INTRO_HOLD = 540
FRAMES = [(INTRO_HOLD-80, 'orbit'), (INTRO_HOLD-60, 'last-orbit'), (INTRO_HOLD-56, 'driver-countdown'),
          (INTRO_HOLD-30, 'one'), (INTRO_HOLD-2, 'before-start'), (INTRO_HOLD+4, 'started')]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', type=Path, default=ROOT / 'build_update/gt2game.exe')
    parser.add_argument('--disc', type=Path, default=ROOT / 'runtime/arcade')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--track', default='seattle')
    parser.add_argument('--with-xr', action='store_true')
    parser.add_argument('--runtime-json', type=Path, default=ROOT / 'build_update/tools/xrsim/xrsim_runtime.json')
    parser.add_argument('--run', action='store_true', help='Run finite captures; otherwise only prepare commands')
    args = parser.parse_args()
    args.game, args.disc, args.runtime_json = (path.resolve() for path in (args.game, args.disc, args.runtime_json))
    output = args.output.resolve()
    if output == args.disc or output.is_relative_to(args.disc):
        parser.error('Output must be separate from installed disc data')
    require_file(args.game)
    require_file(args.disc / 'assets/carobj/dvpgn.cdo.gz')
    if args.with_xr:
        require_file(args.runtime_json)
    output.mkdir(parents=True, exist_ok=False)
    cases = []
    for xr in ([False, True] if args.with_xr else [False]):
        for enabled in (0, 1):
            case = dict(id=f'{"xr" if xr else "desktop"}-countdown-cockpit-{enabled}',
                        car='dvpgn', car_name='Dodge Viper GTS', cockpit=enabled, shots=FRAMES)
            if xr:
                frames = [frame for frame, _ in FRAMES]
                case.update(xr=True, driving=1, captures=frames, xr_script=
                            'resolution 800 800\ncapture ' + ' '.join(map(str, frames)) + '\n'
                            '@0 head pos 0 1.6 0 ypr 0 -12 0\n'
                            '@0 left pos -0.18 1.32 -0.38\n@0 right pos 0.18 1.32 -0.38\n'
                            '@0 left squeeze 1\n@0 right squeeze 1\n'
                            f'@{INTRO_HOLD+10} event exit\n')
            prepare_case(case, args, output)
            # Muting music preserves the native selected intro length. The US
            # Arcade fixture uses 540 fields, verified from its startup log below.
            case['command'].remove('--no-countdown')
            case['command'].remove('--no-hud')
            folder = Path(case['folder'])
            (folder / 'command.json').write_text(json.dumps(dict(command=case['command'], environment=case['env']), indent=2), encoding='utf-8')
            cases.append(case)
    (output / 'plan.json').write_text(json.dumps(dict(game=str(args.game), game_sha256=sha256(args.game),
                                                   disc=str(args.disc), launched=args.run, cases=cases), indent=2), encoding='utf-8')
    if not args.run:
        print(f'Prepared {len(cases)} finite countdown cases: {output}')
        return
    for case in cases:
        print('Running ' + case['id'], flush=True)
        with (Path(case['folder']) / 'game.log').open('w', encoding='utf-8') as log:
            subprocess.run(case['command'], cwd=case['folder'], env=dict(os.environ, **case['env']),
                           stdout=log, stderr=subprocess.STDOUT, timeout=100, check=True,
                           creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        log_text = (Path(case['folder']) / 'game.log').read_text(encoding='utf-8', errors='replace')
        actual_hold = re.search(r'race: .* hold (\d+) fields', log_text)
        assert actual_hold and int(actual_hold.group(1)) == INTRO_HOLD, 'Native intro length changed; retime countdown captures'
        for shot in case['expected']:
            shot['image'] = inspect_png(Path(shot['path']))
        if case.get('xr'):
            with (Path(case['folder']) / 'xrsim_frames.csv').open(newline='', encoding='utf-8') as file:
                rows = {int(row['frame']): row for row in csv.DictReader(file)}
            for frame in case['captures']:
                assert rows[frame]['layers'].startswith('P2'), f'No stereo projection at {frame}'
                assert rows[frame]['result'] == 'ok', f'OpenXR submit failure at {frame}'
        case['status'] = 'passed'
    comparisons = []
    failures = []
    for offset in range(0, len(cases), 2):
        off, on = cases[offset:offset + 2]
        xr = on.get('xr', False)
        for index, (before, after) in enumerate(zip(off['expected'], on['expected'])):
            frame = FRAMES[index // (2 if xr else 1)][0]
            with Image.open(before['path']) as a, Image.open(after['path']) as b:
                difference = max(ImageStat.Stat(ImageChops.difference(a.convert('RGB'), b.convert('RGB'))).mean)
            comparisons.append(dict(case=on['id'], frame=frame, label=after['label'], difference=difference))
            if frame <= INTRO_HOLD-60 and difference > .05:
                failures.append(f'{on["id"]} at {frame}: cockpit changes the external intro camera')
            if frame >= INTRO_HOLD-56 and difference < 2:
                failures.append(f'{on["id"]} at {frame}: cockpit is absent after the driver camera cut')
    write_gallery(output, cases)
    (output / 'report.json').write_text(json.dumps(dict(cases=cases, comparisons=comparisons, failures=failures), indent=2), encoding='utf-8')
    if failures:
        raise SystemExit('\n'.join(failures))
    print(f'PASS: {len(cases)} countdown captures; unchanged external intro and cabin present before GO.')
    print('Visual inspection: ' + str(output / 'gallery.html'))


if __name__ == '__main__':
    main()
