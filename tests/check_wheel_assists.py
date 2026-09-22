"""Check wheel aid defaults and saved direction override in the desktop game."""
import argparse
import os
import pathlib
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--game', required=True, type=pathlib.Path)
    parser.add_argument('--disc', required=True, type=pathlib.Path)
    parser.add_argument('--output', required=True, type=pathlib.Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    settings = output / 'settings.txt'
    settings.write_text('frame_rate=0\nframe_cap=60\n', encoding='ascii')
    settings.with_suffix('.txt.overlay').write_text('hd_assets=0\nprofiler=0\n', encoding='ascii')
    wheel = output / 'wheel-settings.txt'
    wheel.write_text('version 2\noptions 0 0 0 25 10 0\nauto 0 0 0 "" ""\n', encoding='ascii')
    enter = '5:f10,7:down,9:down,11:down,13:enter,15:up,17:up,19:enter,23:up,25:up,27:enter'
    for phase in ('enable', 'reload'):
        script = enter + (',35:down,37:down,39:right' if phase == 'enable' else '')
        script += ',45:esc,47:esc,49:esc,51:esc'
        command = [str(args.game.resolve()), str(args.disc.resolve()), '--race', '--cars', '1',
                   '--no-countdown', '--no-sound', '--no-music', '--settings', str(settings),
                   '--card', str(output / 'card.mcd'), '--script', script,
                   '--shot-at', '32', str(output / (phase + '-initial.png')),
                   '--shot-at', '42', str(output / (phase + '-changed.png')),
                   '--shot', '80', str(output / (phase + '-resumed.png'))]
        with (output / (phase + '.log')).open('w', encoding='utf-8') as log:
            subprocess.run(command, env=dict(os.environ, GT2_NO_FOCUS='1'), stdout=log,
                           stderr=subprocess.STDOUT, check=True, timeout=45,
                           creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        text = wheel.read_text(encoding='utf-8')
        assert 'driving_aids 0 1\n' in text, text
        assert 'ignore_shift_speed 1\n' in text, text
        for suffix in ('initial', 'changed', 'resumed'):
            assert (output / (phase + '-' + suffix + '.png')).stat().st_size > 1000
    print('PASS: TCS 0 / Weak defaults; speed override enabled, saved, reloaded; race resumed.')


if __name__ == '__main__':
    main()
