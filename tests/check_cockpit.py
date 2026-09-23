"""Prepare, or explicitly run, isolated GT2 cockpit captures from local disc data."""
import argparse
import csv
import hashlib
import html
import json
import os
from pathlib import Path
import subprocess
import time

from PIL import Image, ImageChops, ImageStat

ROOT = Path(__file__).resolve().parents[1]
CARS = (
    ('sports', 'dvpgn', 'Dodge Viper GTS'),
    ('tall', 'm2pjn', "Mitsubishi Pajero Mini '97"),
    ('compact', 'brmcn', 'Rover Mini Cooper 1.3i'),
    ('mini-racing', 'brm1r', 'Rover Mini Cooper Racing'),
    ('coupe', 'nr32n', 'Nissan Skyline GT-R R32'),
    ('roadster', 'ioeln', 'Lotus Elise'),
    ('focus', 'effgn', 'Ford Focus Ghia'),
    ('focus-rally', 'effwr', 'Ford Focus Rally Car'),
    ('spider', 'iaspn', 'Alfa Spider'),
    ('mr-spider', 't2spn', 'Toyota MR Spider'),
    ('pt-spyder', 'ulpsn', 'Plymouth PT Spyder'),
)


def require_file(path):
    if not path.is_file():
        raise ValueError('Missing local file: ' + str(path))


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def make_cases(include_xr):
    cases = []
    for shape, car, name in CARS:
        for enabled in (0, 1):
            cases.append(dict(id=f'{shape}-cockpit-{enabled}', car=car, car_name=name,
                              cockpit=enabled, shots=[(30, 'settled'), (90, 'final')]))
    base = dict(car=CARS[0][1], car_name=CARS[0][2], cockpit=1)
    cases.extend([
        dict(base, id='sports-camera-cycle', script='31:c,61:c,91:c',
             shots=[(20, 'driver'), (50, 'chase-1'), (80, 'chase-2'), (120, 'driver-return')]),
        dict(base, id='sports-look-back', script='35:v:40',
             shots=[(30, 'forward'), (60, 'back'), (100, 'forward-return')]),
        dict(base, id='sports-steering', script='5:up:95,35:right:40',
             shots=[(25, 'accelerating'), (55, 'steering-right'), (90, 'released')]),
        dict(base, id='sports-no-wheel', wheel=0, shots=[(90, 'final')]),
        dict(base, id='sports-seat-back', seat_back=40, shots=[(90, 'final')]),
        dict(base, id='sports-mirror-off', mirror=0, shots=[(90, 'final')]),
    ])
    if include_xr:
        for shape, car, name in CARS:
            for enabled in (0, 1):
                cases.append(dict(base, car=car, car_name=name, id=f'xr-{shape}-cockpit-{enabled}', cockpit=enabled, xr=True,
                                  captures=[8, 20, 40, 60, 85, 100]))
        cases.append(dict(base, car='ulpsn', car_name='Plymouth PT Spyder', id='xr-pt-spyder-virtual-wheel',
                          cockpit=1, xr=True, driving=1, captures=[8, 20, 40, 60, 85, 100]))
        for car,name in [('ulpsn','PT Spyder'),('effwr','Focus Rally'),('dvpgn','Viper'),
                         ('brm1r','Mini Racing'),('brmcn','Mini Cooper'),('h2mcn','h2mcn glazing check')]:
            cases.append(dict(base, car=car, car_name=name, id=f'xr-inspect-{car}', cockpit=1, xr=True, driving=1,
                              captures=[20,40,60,85,100], xr_script=
                              'resolution 1000 1000\ncapture 20 40 60 85 100\n'
                              '@0 head pos 0 1.6 0 ypr 0 0 0\n'
                              '@30 head ypr -60 -15 0\n@50 head ypr -105 0 0\n'
                              '@75 head ypr 0 -35 0\n@95 head ypr 110 -20 0\n@110 event exit\n'))
        for mirror_scale,enabled in [(100,1),(25,1),(100,0)]:
            cases.append(dict(base,id=f'xr-mirror-{mirror_scale}-{enabled}',cockpit=1,xr=True,
                              cockpit_mirror=enabled,mirror_scale=mirror_scale,profile=True,captures=[20],xr_script=
                              'resolution 1000 1000\ncapture 20\n'
                              '@0 head pos 0 1.6 0 ypr 0 0 0\n@30 event exit\n'))
        cases.append(dict(base, id='xr-cockpit-motion', cockpit=1, xr=True, driving=1,
                          captures=[20,90,180,270], xr_script=
                          'resolution 800 800\ncapture 20 90 180 270\n'
                          '@0 head pos 0 1.6 0 ypr 0 -20 0\n'
                          '@0 left pos -0.18 1.32 -0.38\n@0 right pos 0.18 1.32 -0.38\n'
                          '@5 left squeeze 1\n@5 right squeeze 1\n@8 right trigger 1\n'
                          '@90 left pos -0.18 1.32 -0.38\n@90 right pos 0.18 1.32 -0.38\n'
                          '@120 left pos -0.10 1.47 -0.38 lerp\n@120 right pos 0.10 1.17 -0.38 lerp\n'
                          '@180 left pos 0.10 1.47 -0.38 lerp\n@180 right pos -0.10 1.17 -0.38 lerp\n'
                          '@220 left squeeze 0\n@220 right squeeze 0\n'
                          '@250 right trigger 0\n@280 event exit\n'))
    return cases


def prepare_case(case, args, output):
    folder = output / case['id']
    folder.mkdir()
    settings = folder / 'settings.txt'
    settings.write_text('frame_rate=original\nframe_cap=60\nvsync=0\nmsaa=4\n'
                        'render_scale=100\ntexture_filter=smooth\n'
                        'texture_mapping=perspective\nvr_multiview=1\n', encoding='ascii')
    settings.with_suffix('.txt.overlay').write_text(
        f"hd_assets=0\nprofiler={int(case.get('profile', False))}\ncockpit={case['cockpit']}\n"
        f"cockpit_wheel={case.get('wheel', 1)}\ncockpit_seat_height=0\n"
        f"cockpit_seat_back={case.get('seat_back', 0)}\nvr_hud_mirror={case.get('mirror', 1)}\nvr_driving_mode={case.get('driving', 0)}\n", encoding='ascii')
    with settings.with_suffix('.txt.overlay').open('a',encoding='ascii') as overlay:
        overlay.write(f"cockpit_mirror={case.get('cockpit_mirror',1)}\ncockpit_mirror_scale={case.get('mirror_scale',100)}\n")
    (folder / 'wheel-settings.txt').write_text(
        'version 2\noptions 0 0 0 25 10 0\nauto 0 0 0 "" ""\n', encoding='ascii')
    command = [str(args.game), str(args.disc), '--race', '--track', args.track,
               '--car', case['car'], '--cars', '1', '--no-countdown', '--camera', '0',
               '--view-angle', '1', '--no-sound', '--no-music', '--no-hud',
               '--window', '1280x720', '--settings', str(settings),
               '--card', str(folder / 'card1.mcd'), '--card2', str(folder / 'card2.mcd'),
               '--fake-pad', '0:type=digital']
    if case.get('driving'):
        command=command[:-2]
    env = dict(GT2_NO_FOCUS='1')
    expected = []
    if case.get('xr'):
        script = folder / 'xr-input.txt'
        script.write_text(case.get('xr_script',
            'resolution 800 800\ncapture 8 20 40 60 85 100\n'
            '@0 head pos 0 1.6 0 ypr 0 0 0\n'
            '@30 head ypr 35 0 0\n'
            '@50 head pos 0.12 1.6 0 ypr 0 0 0\n'
            '@75 head pos 0 1.6 0 ypr 0 -20 0\n'
            '@95 head ypr 0 0 0\n@110 event exit\n'), encoding='ascii')
        command.extend(['--vr', '--xr-deterministic'])
        env.update(XR_RUNTIME_JSON=str(args.runtime_json), XRSIM_SCRIPT=str(script),
                   XRSIM_OUT=str(folder), GT2_XR_MIRROR='0')
        for frame in case['captures']:
            for eye in (0, 1):
                expected.append((f'frame {frame}, eye {eye}',
                                 folder / f'f{frame:06d}_l0_proj_v{eye}.png'))
    else:
        if case.get('script'):
            command.extend(['--script', case['script']])
        for frame, label in case['shots'][:-1]:
            path = folder / f'{frame:03d}-{label}.png'
            command.extend(['--shot-at', str(frame), str(path)])
            expected.append((label, path))
        frame, label = case['shots'][-1]
        path = folder / f'{frame:03d}-{label}.png'
        command.extend(['--shot', str(frame), str(path)])
        expected.append((label, path))
    case.update(folder=str(folder), command=command, env=env,
                expected=[dict(label=label, path=str(path)) for label, path in expected])
    (folder / 'command.json').write_text(json.dumps(dict(command=command, environment=env), indent=2), encoding='utf-8')


def inspect_png(path):
    require_file(path)
    with Image.open(path) as image:
        image.load()
        rgb = image.convert('RGB')
        stats = ImageStat.Stat(rgb)
        if min(rgb.size) < 300 or max(stats.stddev) < 5:
            raise AssertionError('Empty or unexpectedly small screenshot: ' + str(path))
        return dict(width=rgb.width, height=rgb.height, mean=stats.mean,
                    standard_deviation=stats.stddev, sha256=sha256(path))


def inspect_mirror_profile(case, logs_before):
    folder = Path(case['folder'])
    # Read the exact session announced by this process; concurrent tests may also
    # record to the executable's logs directory.
    announced = [line.split('profiler: recording ', 1)[1].strip()
                 for line in (folder / 'game.log').read_text(encoding='utf-8').splitlines()
                 if line.startswith('profiler: recording ')]
    assert len(announced) == 1, (case['id'], announced)
    source = Path(announced[0])
    if not source.is_absolute():
        source = folder / source
    source = source.resolve()
    assert source not in logs_before and source.is_file(), ('Expected a new profiler session', source)
    destination = folder / 'profiler.csv'
    destination.write_bytes(source.read_bytes())
    with destination.open(newline='', encoding='utf-8') as file:
        rows = list(csv.DictReader(file))
    assert all(None not in row and all(value is not None for value in row.values()) for row in rows), destination
    gameplay = [row for row in rows if row['overlay'] == '0' and row['stereo'] == '1'
                and int(row['scene_draws']) > 0 and int(row['frame']) >= 5]
    assert len(gameplay) >= 10, (case['id'], len(gameplay))
    expected_mirror = bool(case['cockpit_mirror'])
    assert all(row['mirror_enabled'] == '1' for row in gameplay), ('HUD mirror master must remain enabled', case['id'])
    assert all((int(row['mirror_source_draws']) > 0) == expected_mirror for row in gameplay), case['id']
    assert all(int(row['cockpit_mirror_enabled']) == int(expected_mirror) for row in gameplay), case['id']
    assert all(int(row['cockpit_mirror_scale']) == case['mirror_scale'] for row in gameplay), case['id']
    fields = ('draws', 'scene_draws', 'mirror_source_draws', 'vertices')
    return dict(source=str(source), path=str(destination), sha256=sha256(destination),
                gameplay_frames=len(gameplay), mirror_expected=expected_mirror,
                hud_mirror_enabled=True, cockpit_mirror_enabled=expected_mirror,
                cockpit_mirror_scale=case['mirror_scale'],
                workload={row['frame']: {key: int(row[key]) for key in fields} for row in gameplay})


def compare_mirror_workload(cases):
    pair = [next((case for case in cases if case['id'] == f'xr-mirror-{scale}-1'
                  and case.get('status') == 'passed'), None) for scale in (100, 25)]
    if not all(pair):
        return None
    original, small = (case['profile_report']['workload'] for case in pair)
    frames = sorted(set(original) & set(small), key=int)
    assert len(frames) >= 10, 'Too few matching mirror workload frames'
    for frame in frames:
        assert original[frame] == small[frame], ('Mirror size changed render workload', frame, original[frame], small[frame])
    return dict(frames_compared=len(frames), sizes_percent=[100, 25],
                unchanged_fields=['draws', 'scene_draws', 'mirror_source_draws', 'vertices'],
                headset_performance_measurement=False)


def write_gallery(output, cases):
    cells = []
    for case in cases:
        pictures = []
        for shot in case['expected']:
            path = Path(shot['path'])
            if path.exists():
                url = path.relative_to(output).as_posix()
                pictures.append(f'<figure><a href="{url}"><img src="{url}"></a>'
                                f'<figcaption>{html.escape(shot["label"])}</figcaption></figure>')
        cells.append(f'<section><h2>{html.escape(case["id"])}</h2>'
                     f'<p>{html.escape(case["car_name"])} ({case["car"]})</p>'
                     '<div class="row">' + ''.join(pictures) + '</div></section>')
    (output / 'gallery.html').write_text(
        '<!doctype html><meta charset="utf-8"><title>GT2 cockpit captures</title>'
        '<style>body{background:#15191f;color:#e5e8eb;font:16px system-ui;margin:24px}'
        '.row{display:flex;gap:12px;flex-wrap:wrap}figure{margin:0;width:460px}'
        'img{width:100%;height:auto}figcaption{padding:6px 0}section{margin:30px 0}'
        'h2{font-size:20px}p{color:#b9c2cf}</style><h1>GT2 cockpit captures</h1>'
        '<p>Local disc assets, isolated settings, no hardware wheel input. '
        'Simulator captures are host rendering checks.</p>' + ''.join(cells), encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', type=Path, default=ROOT / 'build_update/gt2game.exe')
    parser.add_argument('--disc', type=Path, default=ROOT / 'runtime/arcade')
    parser.add_argument('--output', type=Path, required=True, help='New output directory; existing paths are refused')
    parser.add_argument('--track', default='seattle')
    parser.add_argument('--with-xr', action='store_true')
    parser.add_argument('--runtime-json', type=Path, default=ROOT / 'build_update/tools/xrsim/xrsim_runtime.json')
    parser.add_argument('--case', action='append', default=[], help='Restrict to one or more exact case ids')
    parser.add_argument('--run', action='store_true', help='Launch the prepared finite captures; otherwise only write the plan')
    args = parser.parse_args()
    args.game = args.game.resolve()
    args.disc = args.disc.resolve()
    args.runtime_json = args.runtime_json.resolve()
    output = args.output.resolve()
    if output == args.disc or output.is_relative_to(args.disc):
        parser.error('Output must be separate from the installed disc data')
    require_file(args.game)
    if args.with_xr:
        require_file(args.runtime_json)
    for _, car, _ in CARS:
        for ext in ('cdo.gz', 'cdp.gz'):
            require_file(args.disc / 'assets/carobj' / f'{car}.{ext}')
    cases = make_cases(args.with_xr)
    if args.case:
        unknown = set(args.case) - {case['id'] for case in cases}
        if unknown:
            parser.error('Unknown case ids: ' + ', '.join(sorted(unknown)))
        cases = [case for case in cases if case['id'] in args.case]
    output.mkdir(parents=True, exist_ok=False)
    for case in cases:
        prepare_case(case, args, output)
    plan = dict(game=str(args.game), game_sha256=sha256(args.game), disc=str(args.disc),
                cases=cases, launched=args.run)
    (output / 'plan.json').write_text(json.dumps(plan, indent=2), encoding='utf-8')
    if not args.run:
        print(f'Prepared {len(cases)} cases without launching the game: {output / "plan.json"}')
        return
    failures = []
    for case in cases:
        folder = Path(case['folder'])
        print('Running ' + case['id'], flush=True)
        started = time.monotonic()
        try:
            logs_before = {path.resolve() for path in (args.game.parent / 'logs').glob('profiler-*.csv')}
            with (folder / 'game.log').open('w', encoding='utf-8') as log:
                result = subprocess.run(case['command'], cwd=folder, env=dict(os.environ, **case['env']),
                                        stdout=log, stderr=subprocess.STDOUT, timeout=100, check=True,
                                        creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            case['returncode'] = result.returncode
            for shot in case['expected']:
                shot['image'] = inspect_png(Path(shot['path']))
            if case.get('xr'):
                with (folder / 'xrsim_frames.csv').open(newline='', encoding='utf-8') as file:
                    rows = {int(row['frame']): row for row in csv.DictReader(file)}
                for frame in case['captures']:
                    assert rows[frame]['layers'].startswith('P2'), f'No stereo projection at {frame}'
                    assert rows[frame]['result'] == 'ok', f'OpenXR submit failure at {frame}'
            if case.get('profile'):
                case['profile_report'] = inspect_mirror_profile(case, logs_before)
            case['status'] = 'passed'
        except (subprocess.SubprocessError, OSError, AssertionError, ValueError, KeyError) as error:
            case['status'] = 'failed'
            case['error'] = str(error)
            failures.append(case['id'] + ': ' + str(error))
        case['seconds'] = round(time.monotonic() - started, 3)
        print(case['id'] + ': ' + case['status'], flush=True)
    pairs = []
    for shape, _, _ in CARS:
        pair = [next((c for c in cases if c['id'] == f'{shape}-cockpit-{enabled}'
                      and c.get('status') == 'passed'), None) for enabled in (0, 1)]
        if all(pair):
            with Image.open(pair[0]['expected'][-1]['path']) as a, Image.open(pair[1]['expected'][-1]['path']) as b:
                difference = ImageStat.Stat(ImageChops.difference(a.convert('RGB'), b.convert('RGB'))).mean
            pairs.append(dict(shape=shape, mean_absolute_rgb_difference=difference))
            if max(difference) < 1:
                failures.append(shape + ': cockpit on/off images are unexpectedly similar')
    mirror_workload = None
    try:
        mirror_workload = compare_mirror_workload(cases)
    except AssertionError as error:
        failures.append(str(error))
    (output / 'report.json').write_text(json.dumps(dict(cases=cases, pairs=pairs, mirror_workload=mirror_workload,
                                                     failures=failures), indent=2), encoding='utf-8')
    write_gallery(output, cases)
    print('Gallery: ' + str(output / 'gallery.html'))
    if failures:
        raise SystemExit('\n'.join(failures))
    print(f'PASS: {len(cases)} finite render cases. Visual acceptance still requires image inspection.')


if __name__ == '__main__':
    main()
