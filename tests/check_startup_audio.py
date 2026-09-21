"""Check real-time startup playback through the current Windows audio output.

Plays the user's prepared intro, using isolated settings and no game saves.
This hardware-dependent check is intentionally separate from silent CTest runs.
"""
import argparse
import json
import math
from pathlib import Path
import struct
import subprocess
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', type=Path, required=True)
    parser.add_argument('--data-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    movie = args.data_root / 'startup-hd.gtm'
    if not movie.is_file():
        movie = args.data_root / 'startup.gtm'
    with movie.open('rb') as source:
        header = source.read(64)
    assert header[:8] == b'G2MEDIA1' and len(header) == 64, 'Invalid startup movie'
    _, _, frames, fps_num, fps_den = struct.unpack_from('<5I', header, 8)
    assert frames and fps_num and fps_den, 'Invalid movie timing'
    expected = frames * fps_den / fps_num
    assert 0 < expected <= 120, 'Use a startup movie of at most two minutes'
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    saves = output / 'saves'
    saves.mkdir()
    (saves / 'vr-settings.txt').write_text('vr_ps1_intro=1\nhd_assets=1\n', encoding='ascii')
    timeout = expected * 2 + 15
    # Escape is ignored during the unskippable intro; the next pulse exits the disc picker.
    script = ','.join(f'{field}:esc' for field in range(30, math.ceil(timeout * 60), 30))
    command = [str(args.game.resolve()), '--player', '--data-root', str(args.data_root.resolve()),
               '--save-root', str(saves), '--picker-script', script]
    start = elapsed = None
    device_open = False
    with (output / 'game.log').open('w', encoding='utf-8') as log:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, encoding='utf-8', errors='replace')
        watchdog = threading.Timer(timeout, process.kill)
        watchdog.start()
        try:
            for line in process.stdout:
                log.write(line)
                if line.startswith('HD movie:'):
                    start = time.monotonic()
                if line.startswith('audio: waveOut '):
                    device_open = True
                if line.startswith('player: disc picker') and start is not None:
                    elapsed = time.monotonic() - start
            code = process.wait()
        finally:
            watchdog.cancel()
    report = dict(expected_seconds=expected, elapsed_seconds=elapsed, audio_device_open=device_open, exit_code=code)
    (output / 'timing.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    assert code == 0 and device_open and elapsed is not None, report
    assert expected - 0.5 <= elapsed <= expected + max(0.75, expected * 0.05), report
    print(f'PASS: audible startup {elapsed:.3f}s for {expected:.3f}s of media. Logs: {output}')


if __name__ == '__main__':
    main()
