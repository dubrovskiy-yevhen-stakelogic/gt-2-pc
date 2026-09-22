"""Run PCVR rendering/menu checks using the optional local OpenXR simulator."""
import argparse
import csv
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', type=Path, required=True)
    parser.add_argument('--runtime-json', type=Path, required=True)
    parser.add_argument('--disc', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    script = output / 'input.txt'
    script.write_text('''resolution 480 512
capture 8 18 28 40 50 62 80 105
@12 left y 1
@20 left y 0
@22 left squeeze 1
@24 left y 1
@30 left y 0
@32 right squeeze 1
@34 left y 1
@42 left y 0
@42 left squeeze 0
@42 right squeeze 0
@44 left thumbstick_click 1
@44 right thumbstick_click 1
@46 left thumbstick_click 0
@46 right thumbstick_click 0
@52 right b 1
@54 right b 0
@54 left squeeze 0
@54 right squeeze 0
@60 right thumbstick_click 1
@64 right thumbstick_click 0
@72 left squeeze 1
@72 right squeeze 1
@74 left menu 1
@82 left menu 0
@88 right b 1
@92 right b 0
@94 left squeeze 0
@94 right squeeze 0
@96 event focus_loss
@100 event focus_gain
@112 event exit
''', encoding='ascii')
    settings = output / 'settings.txt'
    (output / 'wheel-settings.txt').write_text('version 2\noptions 0 0 0 25 10 0\nauto 0 0 0 "" ""\n', encoding='ascii')
    settings.write_text('vr_multiview=1\n', encoding='ascii')
    settings.with_suffix('.txt.overlay').write_text('vr_driving_mode=1\nhd_assets=0\n', encoding='ascii')
    env = dict(os.environ, XR_RUNTIME_JSON=str(args.runtime_json.resolve()), XRSIM_SCRIPT=str(script),
               XRSIM_OUT=str(output), GT2_XR_MIRROR='0')
    with (output / 'game.log').open('w', encoding='utf-8') as log:
        subprocess.run([str(args.game.resolve()), str(args.disc.resolve()), '--vr', '--xr-deterministic',
                        '--race', '--no-countdown', '--cars', '2', '--no-sound', '--no-music', '--settings', str(settings),
                        '--card', str(output / 'card1.mcd')], env=env, stdout=log,
                       stderr=subprocess.STDOUT, check=True, timeout=120)
    rows = {int(row['frame']): row for row in csv.DictReader((output / 'xrsim_frames.csv').open())}
    for frame in (8, 18, 28, 40, 62, 105):
        assert rows[frame]['layers'].startswith('P2'), f'Expected stereo race at {frame}'
        assert (output / f'f{frame:06d}_l0_proj_v0.png').is_file()
        assert (output / f'f{frame:06d}_l0_proj_v1.png').is_file()
    for frame in (50, 80):
        assert rows[frame]['layers'].startswith('Q'), f'Menu chord failed at {frame}'
        assert (output / f'f{frame:06d}_l0_quad.png').is_file()
    assert all(row['result'] == 'ok' for row in rows.values()), 'OpenXR submission error'
    assert rows[97]['state'] == 'VISIBLE' and rows[105]['state'] == 'FOCUSED', 'Focus transition failed'
    print('PCVR passed: stereo + hands, Y alone, one-grip Y, wheel-grip Y, L3 + R3, both-grip Menu, menu return, clean exit.')
    print('Captures and logs:', output)


if __name__ == '__main__':
    main()
