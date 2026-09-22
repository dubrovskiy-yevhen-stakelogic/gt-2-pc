"""Exercise wheel settings in the actual desktop overlay without wheel hardware."""
import argparse
import pathlib
import re
import subprocess
import os


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--game', required=True, type=pathlib.Path)
    parser.add_argument('--disc', required=True, type=pathlib.Path)
    parser.add_argument('--output', required=True, type=pathlib.Path)
    parser.add_argument('--runtime-json', type=pathlib.Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    settings = output / 'settings.txt'
    settings.write_text('frame_rate=0\nframe_cap=60\n', encoding='ascii')
    settings.with_suffix('.txt.overlay').write_text('hd_assets=0\nprofiler=0\n', encoding='ascii')
    (output / 'wheel-settings.txt').write_text('version 2\noptions 0 0 0 25 10 0\nauto 0 0 0 "" ""\n', encoding='ascii')
    # Disable hardware auto activation during unattended UI checks. Visit the
    # live dashboard, device selection and guided setup, then cancel and resume.
    script = ('5:f10,7:down,9:down,11:down,13:enter,15:up,17:up,19:enter,'
              '23:down,25:right,27:right,29:down,31:right,33:down,35:left,'
              '37:down,39:enter,49:esc,51:down,53:down,55:down,57:down,'
              '59:down,61:enter,65:enter,70:esc,72:esc,74:esc,76:esc,78:esc')
    command = [str(args.game.resolve()), str(args.disc.resolve()), '--race', '--cars', '1', '--no-countdown',
               '--no-sound', '--no-music', '--settings', str(settings), '--card', str(output / 'card.mcd'),
               '--script', script, '--shot-at', '22', str(output / 'wheel-menu.png'),
               '--shot-at', '42', str(output / 'devices.png'), '--shot-at', '62', str(output / 'guide.png'),
               '--shot-at', '68', str(output / 'device-picker.png'),
               '--shot', '88', str(output / 'resumed.png')]
    env = dict(os.environ)
    if args.runtime_json:
        xr_script = output / 'xr-script.txt'
        xr_script.write_text('resolution 480 512\n@500 event exit\n', encoding='ascii')
        env.update(XR_RUNTIME_JSON=str(args.runtime_json.resolve()), XRSIM_SCRIPT=str(xr_script), XRSIM_OUT=str(output))
        command += ['--vr', '--xr-deterministic']
    with (output / 'game.log').open('w', encoding='utf-8') as log:
        subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=150)
    text = (output / 'wheel-settings.txt').read_text(encoding='utf-8')
    assert 'options 0 2 1 20 10 0' in text, text
    assert 'axis 0 "" -1' in text, 'Cancelling capture must not assign a control'
    for filename in ('wheel-menu.png', 'devices.png', 'guide.png', 'device-picker.png', 'resumed.png'):
        assert (output / filename).stat().st_size > 1000, filename
    log = (output / 'game.log').read_text(encoding='utf-8', errors='replace')
    assert ('overlay: opened (VR)' if args.runtime_json else 'overlay: opened (desktop)') in log and 'overlay: closed' in log
    (output / 'wheel-settings.txt').write_text(text.replace('options 0 ', 'options 1 '), encoding='utf-8')
    disconnected = [str(args.game.resolve()), str(args.disc.resolve()), '--race', '--cars', '1', '--no-countdown',
                    '--no-sound', '--no-music', '--settings', str(settings), '--card', str(output / 'card.mcd'),
                    '--drive', '--shot', '100', str(output / 'disconnected-neutral.png')]
    with (output / 'disconnected.log').open('w', encoding='utf-8') as log:
        subprocess.run(disconnected, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=150)
    disconnected_log = (output / 'disconnected.log').read_text(encoding='utf-8', errors='replace')
    throttle = re.findall(r'throttle\s+(\d+) brake', disconnected_log)
    assert throttle and all(value == '0' for value in throttle), disconnected_log
    print('PASS: wheel menu, saved gearbox/FFB settings, cancelled assignment, return to race.')
    print('PASS: missing wheel inputs override held accelerator with zero throttle and neutral.')


if __name__ == '__main__':
    main()
