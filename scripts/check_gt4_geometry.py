"""Source-bound road/wheel colour gate for every GT4 catalogue mapping."""
import argparse
import csv
import json
import os
from pathlib import Path
import subprocess

import check_gt4_cabin_seals as seals


def validate(path, cars):
    required = {(view, steer) for view in range(12) for steer in range(-3, 4)}
    seen = {car: set() for car in cars}
    failures = []
    with path.open(newline='') as stream:
        for row in csv.DictReader(stream):
            car = row['car']
            if car not in seen:
                raise RuntimeError(f'Unknown car: {car}')
            key = int(row['view']), int(row['steer'])
            if key in seen[car] or key not in required:
                raise RuntimeError(f'Invalid or duplicate view: {car} {key}')
            seen[car].add(key)
            if int(row['road_leaks']) or int(row['wheel_leaks']):
                failures.append((car, key, row['road_leaks'], row['wheel_leaks']))
    missing = [car for car, views in seen.items() if views != required]
    if missing or failures:
        raise RuntimeError(f'Incomplete cars: {missing[:12]}; failing views: '
                           f'{len(failures)}; examples: {failures[:12]}')
    return len(cars) * len(required) * 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('disc', 'pack', 'native', 'expansion', 'exe', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--verify-report', action='store_true')
    args = parser.parse_args()
    # Keep the lexical work path: a scoped capture junction may live on H:.
    args.output = Path(os.path.abspath(args.output))
    args.output.relative_to(Path('work').absolute())
    args.output.mkdir(parents=True, exist_ok=True)
    seals.SOURCES += ['tests/gt4_geometry_audit.cpp', 'scripts/check_gt4_geometry.py']
    inputs = seals.provenance(args)
    cars = set(json.loads((args.pack / 'manifest.json').read_text(encoding='utf-8'))['cars'])
    if not cars:
        raise RuntimeError('Empty catalogue')
    report_path = args.output / 'geometry-gate.json'
    csv_path = args.output / 'geometry.csv'
    if args.verify_report:
        report = json.loads(report_path.read_text(encoding='utf-8'))
        if (report.get('format') != 'gt4-road-wheel-gate' or not report.get('passed')
                or report.get('inputs') != inputs or report.get('csv_sha256') != seals.sha(csv_path)):
            raise RuntimeError('Missing, stale or modified road/wheel gate')
        views = validate(csv_path, cars)
        if report.get('cars') != len(cars) or report.get('eye_views') != views:
            raise RuntimeError('Incorrect road/wheel coverage')
        print(f'Current road/wheel gate verified: {len(cars)} cars, {views} eye views')
        return
    env = os.environ.copy()
    for name in tuple(env):
        if name.startswith('GT2_AUDIT_') or name.startswith('GT2_DEBUG_'):
            del env[name]
    env.update(GT2_GT4_NATIVE=str(args.native.absolute()),
               GT2_CAR_EXPANSION=str(args.expansion.absolute()), GT2_AUDIT_WHEEL_COLOR='1')
    with (args.output / 'render.log').open('w') as log:
        result = subprocess.run([str(args.exe.absolute()), str(args.disc.absolute()),
                                 str(args.pack.absolute()), str(args.output)], env=env,
                                stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f'Road/wheel renderer failed: {result.returncode}; see {args.output / "render.log"}')
    views = validate(csv_path, cars)
    if seals.provenance(args) != inputs:
        raise RuntimeError('Geometry inputs changed during the run')
    report_path.write_text(json.dumps(dict(format='gt4-road-wheel-gate', passed=True,
        inputs=inputs, csv_sha256=seals.sha(csv_path), cars=len(cars), eye_views=views), indent=2))
    print(f'Road/wheel gate passed: {len(cars)} cars, {views} eye views')


if __name__ == '__main__':
    main()
