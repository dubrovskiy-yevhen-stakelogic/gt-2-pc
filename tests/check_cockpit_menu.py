"""Check cockpit edits, mirror limits, persistence and return to the race."""
import argparse
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--game', type=Path, required=True)
    parser.add_argument('--disc', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    settings = output / 'settings.txt'
    settings.write_text('frame_rate=0\nframe_cap=60\n', encoding='ascii')
    settings.with_suffix('.txt.overlay').write_text('hd_assets=0\nprofiler=0\n', encoding='ascii')
    (output / 'wheel-settings.txt').write_text('version 2\noptions 0 0 0 25 10 0\nauto 0 0 0 "" ""\n', encoding='ascii')
    enter = '5:f10,7:up,9:up,11:enter'
    expected = {'cockpit': '0', 'cockpit_wheel': '0', 'cockpit_seat_height': '2', 'cockpit_seat_back': '-2',
                'cockpit_mirror': '0', 'cockpit_mirror_scale': '75'}
    phases = (
        ('change', ['right', 'down', 'right', 'down', 'left', 'down', 'right', 'down', 'right', 'down'] + ['left'] * 5),
        ('reload', []),
        ('minimum', ['up'] * 3 + ['left'] * 16),
        ('maximum', ['up'] * 3 + ['right'] * 16),
        ('load-minimum', []),
        ('load-maximum', []),
        ('reset', ['up', 'up', 'enter']),
    )
    for phase, keys in phases:
        if phase.startswith('load-'):
            overlay = settings.with_suffix('.txt.overlay')
            lines = [line for line in overlay.read_text().splitlines() if not line.startswith('cockpit_mirror_scale=')]
            lines.append('cockpit_mirror_scale=' + ('-500' if phase == 'load-minimum' else '500'))
            overlay.write_text('\n'.join(lines) + '\n', encoding='ascii')
        script = enter
        for index, key in enumerate(keys):
            script += f',{15 + index * 2}:{key}'
        menu_frame = 20 + len(keys) * 2
        script += f',{menu_frame + 5}:esc,{menu_frame + 7}:esc'
        command = [str(args.game.resolve()), str(args.disc.resolve()), '--race', '--cars', '1',
                   '--no-countdown', '--camera', '0', '--no-sound', '--no-music', '--settings', str(settings),
                   '--card', str(output / 'card.mcd'), '--fake-pad', '0:type=digital', '--script', script,
                   '--shot-at', str(menu_frame), str(output / (phase + '-menu.png')),
                   '--shot', str(menu_frame + 35), str(output / (phase + '-resumed.png'))]
        with (output / (phase + '.log')).open('w', encoding='utf-8') as log:
            subprocess.run(command, env=dict(os.environ, GT2_NO_FOCUS='1'), stdout=log, stderr=subprocess.STDOUT,
                           check=True, timeout=45, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        saved = dict(line.split('=', 1) for line in settings.with_suffix('.txt.overlay').read_text().splitlines()
                     if '=' in line and not line.startswith('#'))
        if phase in ('minimum', 'load-minimum'):
            expected['cockpit_mirror_scale'] = '25'
        elif phase in ('maximum', 'load-maximum'):
            expected['cockpit_mirror_scale'] = '100'
        if phase == 'reset':
            expected = {'cockpit': '1', 'cockpit_wheel': '1', 'cockpit_seat_height': '0', 'cockpit_seat_back': '0',
                        'cockpit_mirror': '1', 'cockpit_mirror_scale': '100'}
        assert all(saved.get(key) == value for key, value in expected.items()), (phase, saved)
        for suffix in ('menu', 'resumed'):
            assert (output / f'{phase}-{suffix}.png').stat().st_size > 1000
    print('PASS: cockpit, seats, wheel and mirror saved/reloaded/reset; mirror scale clamps to 25-100%; race resumed.')


if __name__ == '__main__':
    main()
