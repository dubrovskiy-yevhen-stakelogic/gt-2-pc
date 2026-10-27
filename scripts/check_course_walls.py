"""Owned-disc wall inventory and stereo regression for extended course visibility.

Enumerate every course, then render every chunk of every affected geometry in
both billboard modes. An independent shoulder witness reproduces Autumn's
foreign-loop fence; ordinary course sweeps use asphalt so valid roadside rails
do not count as intrusions. No retail data belongs in the source package.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with Path(path).open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disc', type=Path, action='append', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--build', type=Path, default=ROOT/'build_gt4_models')
    args = parser.parse_args()
    out = Path(os.path.abspath(args.output))
    out.relative_to(ROOT/'work')
    if out == ROOT/'work':
        parser.error('Use a fresh directory under work')
    out.mkdir(exist_ok=False)
    binaries = {}
    for name in ('gt2wallaudit', 'gt2courseviews'):
        binaries[name] = out/(name+'.exe')
        shutil.copy2(args.build/(name+'.exe'), binaries[name])
    env = {k:v for k,v in os.environ.items() if not k.startswith('GT2_COURSE_')}
    env.update(GT2_WINDOW='hidden', GT2_NO_FOCUS='1')
    report = dict(status='running', binaries={k:digest(v) for k,v in binaries.items()},
                  discs=[], views=[], negative_control=None)

    def save():
        (out/'report.json').write_text(json.dumps(report, indent=2)+'\n')

    def run(name, disc, dest, tail=(), extra=None):
        dest.mkdir(parents=True, exist_ok=True)
        with (dest/'run.log').open('w') as log:
            result = subprocess.run([str(binaries[name]), str(disc), str(dest), *map(str,tail)],
                cwd=ROOT, env=dict(env, **(extra or {})), stdout=log, stderr=subprocess.STDOUT,
                creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        return result.returncode

    check_env = dict(GT2_COURSE_DISTANCE='-1', GT2_COURSE_WALL_CHECK='1', GT2_COURSE_FAILURES_ONLY='1')
    save()
    try:
        seen = set()
        for index, requested in enumerate(args.disc):
            disc = requested.absolute()
            folder = out/f'disc-{index}'
            inventory = folder/'inventory'
            if run('gt2wallaudit', disc, inventory):
                raise RuntimeError('Course wall inventory failed')
            gate = json.loads((inventory/'gate.json').read_text())
            affected = list(csv.DictReader((inventory/'walls.csv').open()))
            if not gate['courses'] or gate['failures']:
                raise RuntimeError('Empty or failed course inventory')
            report['discs'].append(dict(sha256=digest(disc), **gate))
            listing = folder/'catalogue'
            if run('gt2courseviews', disc, listing, ['--list']):
                raise RuntimeError('Course listing failed')
            catalogue = {x['course']:x for x in csv.DictReader((listing/'courses.csv').open())}
            courses = sorted({x['course'] for x in affected})
            if 'autumn' in catalogue and report['negative_control'] is None:
                target = dict(check_env, GT2_COURSE_AUDIT_CHASE='1', GT2_COURSE_FRACTION='.5', GT2_COURSE_WALL_SHOULDERS='1')
                tail = ['autumn', 1, 110, 111, -2, .3]
                for broken in (True, False):
                    dest = folder/('negative-foreign-fence' if broken else 'corrected-foreign-fence')
                    extra = dict(target, **({'GT2_COURSE_RETAIN_MASKED_WALLS':'1'} if broken else {}))
                    code = run('gt2courseviews', disc, dest, tail, extra)
                    result = json.loads((dest/'gate.json').read_text())
                    if result['poses'] != 2 or code != int(broken) or bool(result['failures']) != broken:
                        raise RuntimeError('Foreign-fence negative/corrected witness failed')
                    report['negative_control' if broken else 'corrected_control'] = result
            for course in courses:
                entry = catalogue[course]
                if entry['content_key'] in seen:
                    continue
                seen.add(entry['content_key'])
                for camera in ('driver', 'chase'):
                    dest = folder/(course+'-'+camera)
                    extra = dict(check_env)
                    if camera == 'chase':
                        extra.update(GT2_COURSE_AUDIT_CHASE='1', GT2_COURSE_FRACTION='.5')
                    code = run('gt2courseviews', disc, dest, [course,1], extra)
                    result = json.loads((dest/'gate.json').read_text())
                    rows = list(csv.DictReader((dest/'views.csv').open()))
                    expected = {(str(c),str(m)) for c in range(int(entry['chunks'])) for m in range(2)}
                    actual = {(r['chunk'],r['mode']) for r in rows}
                    if actual != expected or len(rows) != len(expected) or result['poses'] != len(expected):
                        raise RuntimeError('Incomplete or duplicate course camera sweep')
                    report['views'].append(dict(course=course, camera=camera, content=entry['content_key'], **result))
                    save()
                    if code or result['failures']:
                        raise RuntimeError('Extended scenery occludes asphalt: '+course+' '+camera)
            save()
        if report['negative_control'] is None:
            raise RuntimeError('Autumn Ring is required for the visible negative control')
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        save()


if __name__ == '__main__':
    main()
