"""Exercise the unified desktop/PCVR disc picker against locally installed discs."""
import argparse
import csv
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', type=Path, required=True)
    parser.add_argument('--data-root', type=Path, required=True)
    parser.add_argument('--runtime-json', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    saves = output / 'saves'
    saves.mkdir()
    common = saves / 'vr-settings.txt'
    common.write_text('vr_ps1_intro=0\nhd_assets=0\nvr_multiview=1\n', encoding='ascii')
    base = [str(args.game.resolve()), '--data-root', str(args.data_root.resolve()),
            '--save-root', str(saves), '--no-sound', '--no-music']

    def run(name, options, env=None):
        with (output / (name + '.log')).open('w', encoding='utf-8') as log:
            subprocess.run(base + options, env=env, stdout=log, stderr=subprocess.STDOUT,
                           check=True, timeout=150)
        return (output / (name + '.log')).read_text(encoding='utf-8', errors='replace')

    # Change shared HUD/media preferences and a disc-specific cheat using the actual flat overlay.
    script = ('5:f10,7:enter,9:down,11:down,13:down,15:down,17:down,19:down,21:down,23:down,'
              '25:right,27:esc,29:down,31:enter,33:right,35:esc,37:down,39:down,41:enter,43:right,'
              '45:down,47:right,49:esc,51:esc')
    log = run('desktop-arcade', ['--picker-script', '5:enter', '--picker-shot', str(output/'picker.png'),
        '--race', '--cars', '1', '--no-countdown', '--script', script,
        '--shot-at', '48', str(output/'hud-menu.png'), '--shot', '65', str(output/'race.png')])
    assert 'player: selected arcade' in log and 'overlay: opened (desktop)' in log
    assert 'player: PlayStation intro' not in log
    text = common.read_text()
    assert 'hd_assets=1' in text and 'vr_hud_map=0' in text and 'units=mph' in text, text
    saved = dict(line.split('=', 1) for line in text.splitlines() if '=' in line and not line.startswith('#'))
    assert saved['vr_render_scale'] == '130' and saved['vr_hud_gauges'] == '0' and saved['profiler'] == '0', saved
    assert 'unlock_' not in text, 'Cheats leaked into shared preferences'
    assert 'unlock_courses=1' in (saves/'arcade/settings.txt.overlay').read_text()
    assert (output/'picker.png').is_file() and (output/'hud-menu.png').is_file()

    log = run('desktop-simulation', ['--picker-script', '3:down,5:enter', '--title-frames', '12', '--no-movies'])
    assert 'player: selected simulation' in log
    assert (saves/'last-disc.txt').read_text().strip() == 'simulation'
    assert common.read_text() == text, 'Disc selection changed shared settings'

    log = run('remember-and-cancel', ['--picker-script', '4:esc'])
    assert 'disc picker (desktop), 2 installed disc(s)' in log and 'player: selected' not in log
    assert (saves/'last-disc.txt').read_text().strip() == 'simulation'

    xr = output/'xr'
    xr.mkdir()
    xr_script = xr/'input.txt'
    xr_script.write_text('resolution 480 512\ncapture 2 25\n@40 event exit\n', encoding='ascii')
    env = dict(os.environ, XR_RUNTIME_JSON=str(args.runtime_json.resolve()), XRSIM_SCRIPT=str(xr_script),
               XRSIM_OUT=str(xr), GT2_XR_MIRROR='0')
    log = run('vr-simulation', ['--vr', '--xr-deterministic', '--picker-script', '5:enter',
        '--race', '--cars', '1', '--no-countdown'], env)
    assert 'player: selected simulation' in log and 'using running SteamVR' not in log
    rows = {int(r['frame']): r for r in csv.DictReader((xr/'xrsim_frames.csv').open())}
    assert rows[2]['layers'].startswith('Q'), 'Disc picker must be a theatre screen'
    assert rows[25]['layers'].startswith('P2'), 'Selected game must switch to stereo'
    assert all(r['result'] == 'ok' for r in rows.values())
    print('PASS: desktop/VR picker, both discs, remembered choice, cancel, shared HUD/media settings, isolated cheats, stereo transition.')
    print('Logs and screenshots:', output)


if __name__ == '__main__':
    main()
