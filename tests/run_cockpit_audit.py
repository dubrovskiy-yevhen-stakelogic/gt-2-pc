"""Run the local cockpit corpus audit and record the exact executable used."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('disc', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--executable', type=Path, default=ROOT / 'build_update' / 'gt2cockpitaudit.exe')
    parser.add_argument('--car', help='Optional model ID substring filter')
    args = parser.parse_args()
    output = args.output.resolve()
    output.relative_to((ROOT / 'work').resolve())
    executable = args.executable.resolve()
    disc = args.disc.resolve()
    if not executable.is_file() or not disc.exists():
        parser.error('The executable and local disc input must exist')
    if any((output / name).exists() for name in ('audit.csv', 'summary.json', 'run-manifest.json')):
        parser.error('Use a fresh output directory to avoid mixing different audit runs')
    output.mkdir(parents=True, exist_ok=True)
    command = [str(executable), str(disc), str(output)]
    if args.car:
        command.append(args.car)
    manifest = dict(command=command, executable=str(executable), executable_sha256=sha256(executable),
                    started_utc=datetime.now(timezone.utc).isoformat(),
                    diagnostic_geometry_only=True, headset_performance_measurement=False)
    result = subprocess.run(command, cwd=ROOT, check=False)
    manifest.update(returncode=result.returncode, finished_utc=datetime.now(timezone.utc).isoformat(),
                    executable_sha256_after=sha256(executable))
    manifest['executable_unchanged'] = manifest['executable_sha256'] == manifest['executable_sha256_after']
    (output / 'run-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    if not manifest['executable_unchanged']:
        raise SystemExit('Audit executable changed during the run; repeat with a stable build')
    raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
