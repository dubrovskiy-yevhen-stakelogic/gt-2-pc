"""Capture owned GT4 reference frames in an isolated, hidden PCSX2 profile.

No global keyboard input is sent. Messages address only this runner's emulator
windows. All saves, snapshots and state changes stay under the given work path.
Steps JSON: [{"at": seconds, "key": "cross"|"start"|"up"|"down"|"left"|
"right"|"circle"|"snapshot"|"save", "hold": seconds, "label": "optional"}].
For a repeatable car/pose, use --state and record that state hash in the report.
"""
import argparse
import configparser
import ctypes
from ctypes import wintypes
import hashlib
import json
import shutil
import subprocess
import time
from pathlib import Path


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('emulator', 'iso', 'config', 'output', 'steps'):
        p.add_argument('--'+name, type=Path, required=True)
    p.add_argument('--state', type=Path)
    p.add_argument('--card', type=Path, help='Private test card below workspace/work; copied into the isolated profile')
    p.add_argument('--blank-card', action='store_true', help='Create a new unformatted private PS2 card')
    p.add_argument('--duration', type=float, default=90)
    a = p.parse_args()
    out = a.output.resolve()
    workspace_work = Path(__file__).resolve().parents[1]/'work'
    if not out.is_relative_to(workspace_work) or out.exists():
        p.error('Choose a fresh output below the workspace work directory')
    if a.duration > 600 or a.duration <= 0:
        p.error('duration must be between 0 and 600 seconds')
    from gt4_iso import inventory
    with a.iso.open('rb') as f:disc=inventory(f)
    expected=json.loads(Path(__file__).with_name('gt4_profiles.json').read_text())['discTocSha256']
    if disc['toc_fingerprint_sha256']!=expected:p.error('Unverified GT4 disc revision')
    # This Windows build appends its PCSX2 subdirectory to -datapath.
    data_path = out/'profile'
    profile = data_path/'PCSX2'
    (profile/'inis').mkdir(parents=True)
    if a.card and a.blank_card:p.error('Choose card or blank-card')
    if a.card and not a.card.resolve().is_relative_to(workspace_work):
        p.error('Only a private test card below work may be attached')
    if a.card or a.blank_card:
        (profile/'memcards').mkdir()
        card=profile/'memcards/reference.ps2'
        if a.card:shutil.copyfile(a.card,card)
        else:card.write_bytes(b'\xff'*(528*16384))
    cfg = configparser.ConfigParser(interpolation=None, strict=False)
    cfg.optionxform = str
    cfg.read(a.config, encoding='utf-8-sig')
    # Do not copy account settings or credentials into the private test profile.
    for section in list(cfg.sections()):
        if section not in ('UI', 'Folders', 'EmuCore', 'EmuCore/CPU', 'EmuCore/Speedhacks',
                           'EmuCore/GS', 'EmuCore/SPU2', 'Filenames', 'Framerate', 'MemoryCards'):
            cfg.remove_section(section)
    for section in ('UI', 'EmuCore', 'EmuCore/GS', 'SPU2/Output', 'MemoryCards', 'InputSources', 'Pad', 'Pad1', 'Hotkeys', 'Logging'):
        if not cfg.has_section(section): cfg.add_section(section)
    cfg['UI'].update(StartFullscreen='false', PauseOnFocusLoss='false',
                     SetupWizardIncomplete='false', ConfirmShutdown='false')
    cfg['EmuCore'].update(SaveStateOnShutdown='false', EnableDiscordPresence='false',
                          EnablePINE='false', EnableCheats='false')
    cfg['EmuCore/GS'].update(Renderer='1', OsdShowMessages='false')
    cfg['SPU2/Output'].update(StandardVolume='0', FastForwardVolume='0')
    # No original card is attached or writable by the reference process.
    cfg['MemoryCards'].update(Slot1_Enable='true' if a.card or a.blank_card else 'false',
                              Slot1_Filename='reference.ps2', Slot2_Enable='false')
    cfg['InputSources'].update(Keyboard='true',Mouse='false',SDL='false')
    cfg['Pad'].update(MultitapPort1='false',MultitapPort2='false')
    cfg['Pad1'].update(Type='DualShock2', Up='Keyboard/Up', Down='Keyboard/Down',
                       Left='Keyboard/Left', Right='Keyboard/Right',
                       Cross='Keyboard/F7', Circle='Keyboard/F6', Start='Keyboard/Return')
    cfg['Hotkeys'].update(Screenshot='Keyboard/F8', SaveStateToSlot='Keyboard/F1')
    cfg['Logging'].update(EnableFileLogging='true', EnableSystemConsole='false')
    with (profile/'inis/PCSX2.ini').open('w', encoding='utf-8') as f: cfg.write(f)
    steps = json.loads(a.steps.read_text(encoding='utf-8-sig'))
    keys = {'up':0x26, 'down':0x28, 'left':0x25, 'right':0x27,
            'cross':0x76, 'circle':0x75, 'start':0x0d, 'snapshot':0x77, 'save':0x70}
    for step in steps:
        if step['key'] not in keys or not 0 <= step['at'] <= a.duration:
            raise ValueError('Unsupported step: '+str(step))
    user32 = ctypes.WinDLL('user32', use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.IsWindowVisible.argtypes = [wintypes.HWND]
    user32.MapVirtualKeyW.argtypes = [wintypes.UINT,wintypes.UINT]
    def key_bits(key):
        return 1 | (user32.MapVirtualKeyW(key,0)<<16) | ((1<<24) if key in (0x25,0x26,0x27,0x28) else 0)
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = subprocess.SW_HIDE
    command = [str(a.emulator.resolve()), '-datapath', str(data_path), '-nogui',
               '-nofullscreen', '-fastboot', '-logfile', str(out/'emulator.log')]
    if a.state: command += ['-statefile', str(a.state.resolve())]
    command += ['--', str(a.iso.resolve())]
    report = {'emulator':str(a.emulator.resolve()), 'emulatorSha256':digest(a.emulator),
              'iso':str(a.iso.resolve()), 'steps':steps, 'stateSha256':digest(a.state) if a.state else None,
              'isoSize':a.iso.stat().st_size,'isoTocSha256':disc['toc_fingerprint_sha256'],
              'privateMemoryCardAttached':bool(a.card or a.blank_card),
              'originalMemoryCardAttached':False, 'globalInputSent':False, 'audioMuted':True, 'captures':[]}
    with (out/'console.log').open('w', encoding='utf-8') as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                   startupinfo=startup, creationflags=subprocess.CREATE_NO_WINDOW)
        started = time.monotonic()
        done = set()
        pending = []
        try:
            while process.poll() is None and time.monotonic()-started < a.duration:
                windows = []
                @callback_type
                def collect(hwnd, param):
                    pid = wintypes.DWORD()
                    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                    if pid.value == process.pid:
                        windows.append(hwnd)
                        if user32.IsWindowVisible(hwnd): user32.ShowWindow(hwnd, 0)
                    return True
                user32.EnumWindows(collect, 0)
                # Controller events belong to Qt's child display surface;
                # main-window hotkeys alone do not drive the emulated pad.
                children=[]
                @callback_type
                def collect_child(hwnd,param):
                    children.append(hwnd)
                    return True
                for hwnd in list(windows):user32.EnumChildWindows(hwnd,collect_child,0)
                windows+=children
                now = time.monotonic()-started
                for at, hwnd, key in list(pending):
                    if now >= at:
                        user32.PostMessageW(hwnd, 0x101, key, (1<<31)|(1<<30)|key_bits(key))
                        pending.remove((at, hwnd, key))
                for i, step in enumerate(steps):
                    if i in done or now < step['at'] or not windows: continue
                    done.add(i)
                    for hwnd in windows:
                        key = keys[step['key']]
                        user32.PostMessageW(hwnd, 0x100, key, key_bits(key))
                        pending.append((now+step.get('hold', .12), hwnd, key))
                time.sleep(.05)
        finally:
            if process.poll() is None: process.terminate()
            process.wait(timeout=10)
        report['exitCode'] = process.returncode
        report['executedSteps'] = sorted(done)
    for path in sorted(profile.rglob('*.png')):
        report['captures'].append({'path':str(path.relative_to(out)), 'sha256':digest(path)})
    report['status'] = 'captured' if report['captures'] else 'no_reference_frames'
    (out/'report.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(report['status'], len(report['captures']), flush=True)
    if not report['captures']: raise SystemExit(1)


if __name__ == '__main__': main()
