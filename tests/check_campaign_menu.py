"""Exercise campaign reset/restore through the real overlay on disposable saves."""
import argparse
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
    card = output / 'card.mcd'

    def run(name, script, end, shots):
        command = [str(args.game.resolve()), str(args.disc.resolve()), '--menu', '--no-sound', '--no-music',
                   '--settings', str(settings), '--card', str(card), '--save-out', str(card),
                   '--menu-script', script, '--menu-frames', str(end)]
        if card.exists():
            command += ['--career', str(card)]
        for field, label in shots:
            command += ['--shot-at', str(field), str(output / f'{name}-{label}.png')]
        with (output / f'{name}.log').open('w', encoding='utf-8') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=90)
        for _, label in shots:
            assert (output / f'{name}-{label}.png').stat().st_size > 1000

    run('seed', '5:f10,7:down,9:enter,11:down,13:down,15:down,17:enter,23:esc,25:esc', 30, [])
    original = card.read_bytes()
    open_manager = '5:f10,7:down,9:down,11:down,13:down,15:down,17:down,19:down,21:down,23:enter,'
    run('cancel', open_manager + '25:down,27:enter,31:enter,35:esc,37:esc', 42, [(29, 'default-cancel')])
    assert card.read_bytes() == original, 'Default confirmation must cancel without changing the save'
    backups = pathlib.Path(str(card) + '.campaign-backups')
    assert not backups.exists(), 'Cancel must not create reset backups'
    # Screenshots can stall rendering for >250ms, which deliberately cancels a
    # destructive hold. Capture only before or after the uninterrupted hold.
    run('reset', open_manager + '25:down,27:enter,29:down,40:enter:300,350:enter,360:esc,362:esc,364:esc', 380,
        [(31, 'warning'), (345, 'saved')])
    assert card.read_bytes() != original, 'Confirmed reset must replace the career'
    assert (backups / '00000001' / 'original.bin').read_bytes() == original
    fresh = card.read_bytes()
    run('restore', open_manager + '25:up,27:enter,29:down,40:enter:300,350:enter,360:esc,362:esc,364:esc', 380,
        [(31, 'warning'), (345, 'saved')])
    assert card.read_bytes() == original, 'Restore must recover the pre-reset career'
    assert (backups / '00000002' / 'original.bin').read_bytes() == fresh
    print('PASS: real overlay default cancel, timed reset, persistent backup and restore; screenshots:', output)


if __name__ == '__main__':
    main()
