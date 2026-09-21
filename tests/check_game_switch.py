"""Switch both installed discs in one PCVR process, using the local OpenXR simulator."""
import argparse
import csv
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('game', 'data-root', 'runtime-json', 'output'):
        parser.add_argument('--' + option, type=Path, required=True)
    parser.add_argument('--title', action='store_true', help='Exercise the normal game shell instead of a standalone race')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    saves = output / 'saves'
    saves.mkdir()
    (saves / 'vr-settings.txt').write_text('vr_ps1_intro=0\nhd_assets=0\nvr_multiview=1\n')
    cards = {}
    for mode in ('arcade', 'simulation'):
        target = saves / mode
        target.mkdir()
        # A race does not load or write a career card. Distinct sentinels detect replacement/mixing.
        card = target / 'card1.mcd'
        card.write_bytes((mode + '-preserve-card\n').encode())
        cards[card] = hashlib.sha256(card.read_bytes()).hexdigest()
        (target / 'settings.txt.overlay').write_text('unlock_courses=' + ('1' if mode == 'arcade' else '0') + '\n')
    xr = output / 'xr'
    xr.mkdir()
    script = output / 'input.txt'
    script.write_text('resolution 480 512\ncapture 2 8 41 47\n@33 right trigger 1\n@35 right trigger 0\n@300 event exit\n')
    env = dict(os.environ, XR_RUNTIME_JSON=str(args.runtime_json.resolve()), XRSIM_SCRIPT=str(script),
               XRSIM_OUT=str(xr), GT2_XR_MIRROR='0')
    # Enable the PS1 startup after entering a disc, then cancel Change game once and confirm.
    # The next picker must still skip the startup, even though the saved preference is now enabled.
    menu = ('5:f10,7:enter,9:down,11:down,13:down,15:down,17:down,19:down,21:down,23:down,25:down,'
            '29:esc,31:down,33:down,35:down,37:down,39:enter,41:enter,43:enter,45:up,47:enter')
    log_path = output / 'game.log'
    with log_path.open('w', encoding='utf-8') as log:
        process = subprocess.Popen([str(args.game.resolve()), '--data-root', str(args.data_root.resolve()),
            '--save-root', str(saves), '--vr', '--xr-deterministic', '--picker-script', '2:down,4:enter',
            *(['--title', '--no-movies'] if args.title else ['--race', '--cars', '1', '--no-countdown']),
            '--no-sound', '--no-music', '--script', menu],
            env=env, stdout=log, stderr=subprocess.STDOUT)
        completed = 0
        deadline = time.monotonic() + 150
        try:
            while process.poll() is None:
                text = log_path.read_text(encoding='utf-8', errors='replace')
                requested = text.count('player: change game requested')
                if requested > completed:
                    assert requested == completed + 1 and requested <= 2, text
                    # The old session has not yet been destroyed: archive its frame evidence.
                    snapshot = output / ('session-' + str(requested))
                    shutil.copytree(xr, snapshot)
                    completed = requested
                    if completed == 1:
                        assert 'vr_ps1_intro=1' in (saves / 'vr-settings.txt').read_text()
                    if completed == 2:
                        # The next instance reads this script. End the third race before its menu script.
                        script.write_text('resolution 480 512\ncapture 2 8\n@9 event exit\n')
                if time.monotonic() > deadline:
                    raise TimeoutError('Disc switch did not finish')
                time.sleep(.005)
            assert process.returncode == 0, log_path.read_text(errors='replace')
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
    text = log_path.read_text(encoding='utf-8', errors='replace')
    selections = [line.split()[-1] for line in text.splitlines() if line.startswith('player: selected ')]
    assert selections == ['simulation', 'arcade', 'simulation'], selections
    assert completed == 2 and text.count('disc picker (VR)') == 3, text
    assert 'player: PlayStation intro' not in text
    assert (saves / 'last-disc.txt').read_text().strip() == 'simulation'
    for card, digest in cards.items():
        assert hashlib.sha256(card.read_bytes()).hexdigest() == digest, card
    for mode, unlock in [('arcade', '1'), ('simulation', '0')]:
        assert 'unlock_courses=' + unlock in (saves / mode / 'settings.txt.overlay').read_text()
    for directory in [output / 'session-1', output / 'session-2', xr]:
        rows = list(csv.DictReader((directory / 'xrsim_frames.csv').open()))
        if not args.title:
            assert any(r['layers'].startswith('P2') for r in rows), directory
        assert any(r['layers'].startswith('Q') for r in rows), directory
        assert all(r['result'] == 'ok' for r in rows), directory
    print('PASS: Simulation -> Arcade -> Simulation in one process; menu cancellation, startup skipped, isolated cards/cheats, clean exit.',
          'Normal game shell.' if args.title else 'Three stereo races.')
    print('Evidence:', output)


if __name__ == '__main__':
    main()
