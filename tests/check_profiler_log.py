"""Verify that the visible profiler toggle starts/stops persistent CSV sessions."""
import argparse
import csv
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', type=Path, required=True)
    parser.add_argument('--disc', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    game, disc, output = args.game.resolve(), args.disc.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    logs = game.parent / 'logs'
    before = set(logs.glob('profiler-*.csv'))
    settings = output / 'settings.txt'
    settings.write_text('frame_rate=0\nframe_cap=60\n')
    settings.with_suffix('.txt.overlay').write_text('hd_assets=0\nprofiler=0\n')
    (output / 'wheel-settings.txt').write_text('version 2\noptions 0 0 0 25 10 0\nauto 0 0 0 "" ""\n')
    # Graphics row 6; record gameplay between enable and disable, then enable again.
    enter = '5:f10,7:enter' + ''.join(f',{9+i*2}:down' for i in range(6))
    script = enter + ',23:right,25:esc,27:esc,65:f10,67:enter'
    script += ''.join(f',{69+i*2}:down' for i in range(6))
    script += ',83:right,85:esc,87:esc,100:f10,102:enter'
    script += ''.join(f',{104+i*2}:down' for i in range(6))
    script += ',118:right,120:esc,122:esc'
    command = [str(game), str(disc), '--race', '--cars', '1', '--no-countdown', '--camera', '0',
               '--no-sound', '--no-music', '--settings', str(settings), '--card', str(output/'card.mcd'),
               '--fake-pad', '0:type=digital', '--script', script, '--shot', '160', str(output/'final.png')]
    with (output/'game.log').open('w') as log:
        subprocess.run(command, cwd=output, env=dict(os.environ, GT2_NO_FOCUS='1'), stdout=log,
                       stderr=subprocess.STDOUT, check=True, timeout=90,
                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    created = sorted(set(logs.glob('profiler-*.csv')) - before)
    assert len(created) == 2, created
    for path in created:
        with path.open(newline='') as source:
            rows = list(csv.DictReader(source))
        assert len(rows) > 10, (path, len(rows))
        assert all(None not in row and all(value is not None for value in row.values()) for row in rows)
        assert any(row['overlay'] == '0' and int(row['scene_draws']) > 0 for row in rows)
        assert any(int(row['mirror_source_draws']) > 0 for row in rows)
        assert all(float(row['render_submit_ms']) >= 0 for row in rows)
        assert rows[0]['frame'] == '0' and rows[0]['app_frame_ms'] == '-1.000'
        (output/path.name).write_bytes(path.read_bytes())
    print('PASS: menu toggle created two independent CSV logs with gameplay and mirror workload.')


if __name__ == '__main__':
    main()
