"""Owned-disc backdrop ordering/image sweep and optional Quest GPU regression gate.

No game activity is launched. Captures and reports stay under work/. ADB runs
only gt2renderbench in a unique /data/local/tmp directory on the named device.
"""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import argparse, csv, gzip, hashlib, json, os, re, shutil, statistics, subprocess, time, uuid

ROOT = Path(__file__).resolve().parents[1]
HIDDEN = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def regression(current, reference, ratio=1.10, allowance=.20):
    return current > reference * ratio + allowance


def paired_samples(text):
    phases = re.findall(r'Sky pair phase (\d+) deferred=(\d+)(.*?)(?=Sky pair phase|\Z)', text, re.S)
    if [(int(n), int(changed)) for n, changed, _ in phases] != [(0, 0), (1, 1), (2, 1), (3, 0)]:
        raise RuntimeError('missing or reordered ABBA phases')
    result = []
    for _, changed, output in phases:
        match = re.search(r'GPU avg ([0-9.]+) ms p95 ([0-9.]+) p99 ([0-9.]+); CPU\+GPU ([0-9.]+) ms', output)
        if not match or any(float(match[i]) <= 0 for i in (1, 2, 3)):
            raise RuntimeError('missing positive GPU timestamp measurement')
        result.append(dict(deferred_sky=bool(int(changed)), mean_ms=float(match[1]),
                           p95_ms=float(match[2]), p99_ms=float(match[3])))
    return result


def command(args, *, env=None, timeout=600):
    p = subprocess.run(list(map(str, args)), cwd=ROOT, env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, creationflags=HIDDEN, timeout=timeout)
    return p.returncode, p.stdout.decode('utf-8', 'replace')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disc', action='append', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--audit', type=Path, default=ROOT/'build_gt4_models/gt2courseviews.exe')
    parser.add_argument('--stride', type=int, default=1)
    parser.add_argument('--workers', type=int, default=1, help='Independent host course processes (1-4)')
    parser.add_argument('--resume', action='store_true', help='Resume a stopped host-only run using its immutable audit snapshot')
    parser.add_argument('--adb', type=Path)
    parser.add_argument('--serial')
    parser.add_argument('--bench-directory', type=Path)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--scale', type=int, default=130)
    parser.add_argument('--msaa', type=int, default=2)
    parser.add_argument('--foveation', type=int, default=2)
    parser.add_argument('--frames', type=int, default=180)
    args = parser.parse_args()
    out = args.output.resolve()
    if not out.is_relative_to(ROOT/'work') or out == ROOT/'work':
        parser.error('output must be a new directory under work/')
    if args.stride < 1 or args.frames < 60:
        parser.error('stride >= 1 and frames >= 60 required')
    if any((args.adb, args.serial, args.bench_directory)) and not all((args.adb, args.serial, args.bench_directory)):
        parser.error('--adb, --serial and --bench-directory must be provided together')
    if not 1 <= args.workers <= 4 or (args.adb and (args.workers != 1 or args.resume)):
        parser.error('workers must be 1-4; GPU runs require one worker and a new output')
    saved = json.loads((out/'report.json').read_text()) if args.resume else None
    out.mkdir(parents=True, exist_ok=args.resume)
    snapshot = out/'gt2courseviews.exe'
    if not args.resume:
        shutil.copy2(args.audit, snapshot)
    args.audit = snapshot
    env = dict(os.environ, GT2_WINDOW='hidden', GT2_NO_FOCUS='1', GT2_COURSE_ORDER_CHECK='1',
               GT2_COURSE_FAILURES_ONLY='1', GT2_COURSE_DISTANCE='-1')
    for key in ('GT2_COURSE_LOOK_BACK', 'GT2_COURSE_AUDIT_CAPTURE', 'GT2_COURSE_CAPTURE_EVERY', 'GT2_COURSE_INJECT_SKY_FIRST', 'GT2_COURSE_FRACTION', 'GT2_COURSE_AUDIT_CHASE', 'GT2_COURSE_MAX_DETAIL', 'GT2_COURSE_YAW_OFFSET', 'GT2_COURSE_PITCH'):
        env.pop(key, None)
    config = dict(scale=args.scale, msaa=args.msaa, foveation=args.foveation,
                  multiview=True, smooth=True, frames=args.frames, paired_process=True)
    report = dict(format='gt2-backdrop-regression-v1', audit_sha256=digest(args.audit),
                  config=config, serial=args.serial, catalogue=[], courses=[], failures=[],
                  status='running', physical_headset_fps_verified=False)
    if saved:
        if saved['audit_sha256'] != report['audit_sha256'] or saved['config'] != config or saved['serial'] is not None:
            raise RuntimeError('resume requires the unchanged audit snapshot and configuration')
        if saved.get('stride', args.stride) != args.stride:
            raise RuntimeError('resume stride differs')
        for course in saved['courses']:
            expected = 2 * ((int(course['chunks']) + args.stride - 1) // args.stride)
            if len(course['host']) != 2 or any(x['poses'] != expected for x in course['host']):
                raise RuntimeError('resume has incomplete/different host coverage')
        report['courses'] = saved['courses']
        report['failures'] = saved['failures']
    report['stride'] = args.stride
    report['workers'] = args.workers
    completed = {x['content_key'] for x in report['courses']}
    previous = json.loads(args.baseline.read_text()) if args.baseline else None
    if previous and (previous['config'] != config or previous['serial'] != args.serial or previous['status'] != 'passed'):
        raise RuntimeError('baseline must be a passed run on the same device/settings')
    old = {x['content_key']: x for x in previous['courses']} if previous else {}

    def save():
        (out/'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')

    def adb(*tail):
        code, text = command([args.adb, '-s', args.serial, *tail], timeout=180)
        if code:
            raise RuntimeError(text)
        return text

    unique = {}
    for i, disc in enumerate(args.disc):
        dest = out/f'inventory-{i}'
        code, log = command([args.audit, disc, dest, '--list'], env=env)
        (out/f'inventory-{i}.log').write_text(log)
        if code:
            raise RuntimeError(log)
        for row in csv.DictReader((dest/'courses.csv').open()):
            entry = dict(row, disc=str(disc.resolve()))
            report['catalogue'].append(entry)
            unique.setdefault(row['content_key'], entry)
    report['unique_content_sets'] = len(unique)
    if saved and saved['catalogue'] != report['catalogue']:
        raise RuntimeError('resume disc catalogue changed')
    # Same course name is not enough for deduplication: key includes geometry,
    # track textures, backdrop geometry AND backdrop textures from the disc.
    remote = '/data/local/tmp/gt2-sky-regression-' + uuid.uuid4().hex
    report['remote_directory'] = remote if args.adb else None
    if args.adb:
        report['device_model'] = adb('shell', 'getprop', 'ro.product.model').strip()
        report['device_build'] = adb('shell', 'getprop', 'ro.build.fingerprint').strip()
        report['bench_sha256'] = digest(args.bench_directory/'gt2renderbench')
        adb('shell', 'mkdir', '-p', remote)
        for name in ('gt2renderbench', 'libopenxr_loader.so'):
            adb('push', args.bench_directory/name, remote+'/'+name)
        adb('shell', 'chmod', '700', remote+'/gt2renderbench')
    def host_course(number, entry):
        name = entry['course']; dest = out/f'{number:03d}-{name}'
        measurements = []
        for reverse in (False, True):
            direction = 'reverse-view' if reverse else 'forward-view'
            runenv = dict(env)
            if reverse:
                runenv['GT2_COURSE_LOOK_BACK'] = '1'
            code, log = command([args.audit, entry['disc'], dest/direction, name, args.stride], env=runenv, timeout=1800)
            dest.mkdir(exist_ok=True)
            (dest/(direction+'.log')).write_text(log)
            gate = json.loads((dest/direction/'gate.json').read_text()) if (dest/direction/'gate.json').exists() else {'failures': 1, 'poses': 0}
            measurements.append(dict(direction=direction, exit_code=code, **gate))

        return measurements

    pool = ThreadPoolExecutor(max_workers=args.workers)
    futures = {}
    save()
    try:
        # Deliberately reintroducing the old ordering must fail even when it
        # produces the same pixels. This guards the expensive invisible bug.
        witness = next(x for x in unique.values() if x['backdrop'])
        negative_env = dict(env, GT2_COURSE_INJECT_SKY_FIRST='1')
        code, log = command([args.audit, witness['disc'], out/'negative-sky-first', witness['course'], 1, 0, 1], env=negative_env)
        (out/'negative-sky-first.log').write_text(log)
        negative = json.loads((out/'negative-sky-first/gate.json').read_text())
        report['negative_control'] = negative
        if code != 1 or not negative['failures']:
            raise RuntimeError('sky-first negative control was not rejected')
        futures = {key: pool.submit(host_course, number, entry)
                   for number, (key, entry) in enumerate(unique.items()) if key not in completed}
        for number, (key, entry) in enumerate(unique.items()):
            if key in completed:
                continue
            name = entry['course']; dest = out/f'{number:03d}-{name}'
            report['current'] = dict(index=number, course=name, phase='host')
            save()
            print(f'{number+1}/{len(unique)} {name}: host sweep', flush=True)
            result = dict(entry, gpu=[], host=[])
            result['host'] = futures[key].result()
            for gate in result['host']:
                if gate['exit_code'] or gate['failures']:
                    report['failures'].append(dict(course=name, gate=gate['direction'], result=gate))
            if args.adb and entry['backdrop']:
                for chunk in sorted({0, int(entry['chunks'])//3, int(entry['chunks'])*2//3}):
                    capture_dir = dest/f'capture-{chunk}'
                    capenv = dict(env, GT2_COURSE_AUDIT_CAPTURE='1')
                    code, log = command([args.audit, entry['disc'], capture_dir, name, 1, chunk, chunk+1], env=capenv)
                    (dest/f'capture-{chunk}.log').write_text(log)
                    if code:
                        report['failures'].append(dict(course=name, chunk=chunk, gate='capture', exit_code=code))
                        continue
                    cap = capture_dir/f'{chunk}.gt2cap'
                    row = dict(chunk=chunk, capture_sha256=digest(cap), samples=[])
                    report['current'] = dict(index=number, course=name, phase='gpu', chunk=chunk)
                    save()
                    adb('push', cap, remote+'/scene.gt2cap')
                    row['battery_before'] = adb('shell', 'dumpsys', 'battery')
                    text = adb('shell', 'env', 'LD_LIBRARY_PATH='+remote, 'GT2_BENCH_MULTIVIEW=1', 'GT2_BENCH_SKY_PAIR=1',
                               remote+'/gt2renderbench', remote+'/scene.gt2cap', args.scale, args.msaa,
                               args.frames, "''", 1, 0, args.foveation)
                    (dest/f'gpu-{chunk}-abba.log').write_text(text)
                    row['samples'] = paired_samples(text)
                    for changed, label in ((False, 'before'), (True, 'after')):
                        samples = [x for x in row['samples'] if x['deferred_sky'] == changed]
                        row[label+'_ms'] = statistics.median(x['mean_ms'] for x in samples)
                        row[label+'_p95_ms'] = statistics.median(x['p95_ms'] for x in samples)
                    row['battery_after'] = adb('shell', 'dumpsys', 'battery')
                    row['saved_ms'] = row['before_ms'] - row['after_ms']
                    if regression(row['after_ms'], row['before_ms'], 1.05) or regression(row['after_p95_ms'], row['before_p95_ms']):
                        report['failures'].append(dict(course=name, chunk=chunk, gate='paired_gpu', before_ms=row['before_ms'], after_ms=row['after_ms']))
                    if previous:
                        reference = next((x for x in old.get(key, {}).get('gpu', []) if x['chunk'] == chunk), None)
                        if reference is None:
                            report['failures'].append(dict(course=name, chunk=chunk, gate='baseline_missing'))
                        elif regression(row['after_ms'], reference['after_ms']) or regression(row['after_p95_ms'], reference['after_p95_ms']):
                            report['failures'].append(dict(course=name, chunk=chunk, gate='historical_gpu', reference_ms=reference['after_ms'], current_ms=row['after_ms']))
                    result['gpu'].append(row)
                    # Keep a reproducible compressed frame, not hundreds of MB
                    # of unused zero-filled renderer buffers per camera pose.
                    with cap.open('rb') as source, gzip.open(str(cap)+'.gz', 'wb', compresslevel=1) as target:
                        shutil.copyfileobj(source, target)
                    with gzip.open(str(cap)+'.gz', 'rb') as check:
                        if hashlib.file_digest(check, 'sha256').hexdigest() != row['capture_sha256']:
                            raise RuntimeError('compressed capture checksum mismatch')
                    cap.unlink()
            report['courses'].append(result)
            save()
            print(f'{number+1}/{len(unique)} {name}: {sum(x["poses"] for x in result["host"])} poses, {len(report["failures"])} failures', flush=True)
        report.pop('current', None)
        report['status'] = 'failed' if report['failures'] else 'passed'
        save()
    except BaseException as error:
        report['status'] = 'interrupted'
        report['error'] = str(error)
        save()
        raise
    finally:
        pool.shutdown(wait=True, cancel_futures=True)
        if args.adb:
            # Only our unique, validated directory; no app data is touched.
            adb('shell', 'rm', '-f', remote+'/scene.gt2cap', remote+'/shot.png', remote+'/gt2renderbench', remote+'/libopenxr_loader.so')
            adb('shell', 'rmdir', remote)
    return 1 if report['failures'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
