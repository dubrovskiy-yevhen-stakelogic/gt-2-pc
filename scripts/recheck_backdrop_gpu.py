"""Bounded follow-up of paired Quest GPU failures; preserves the original report."""
import argparse
import copy
import gzip
import json
import os
from pathlib import Path
import shutil
import statistics
import uuid

from check_backdrop_regression import ROOT, command, digest, paired_samples, regression


def summarize(samples):
    result = {}
    for changed, label in ((False, 'before'), (True, 'after')):
        rows = [x for x in samples if x['deferred_sky'] == changed]
        if len(rows) != 2:
            raise RuntimeError('Each repetition requires exactly one complete ABBA run')
        result[label+'_ms'] = statistics.median(x['mean_ms'] for x in rows)
        result[label+'_p95_ms'] = statistics.median(x['p95_ms'] for x in rows)
    result['passed'] = not (regression(result['after_ms'], result['before_ms'], 1.05) or
                            regression(result['after_p95_ms'], result['before_p95_ms']))
    return result


def disposition(repetitions):
    # Never retry until a good sample appears: all three scheduled repetitions
    # must pass the unchanged thresholds. A missing repetition also fails.
    return len(repetitions) == 3 and all(x['passed'] for x in repetitions)


def retained_capture(source, index, course, chunk):
    return source.parent/f'{index:03d}-{course}'/f'capture-{chunk}'/f'{chunk}.gt2cap.gz'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--report', type=Path, required=True)
    p.add_argument('--adb', type=Path, required=True)
    p.add_argument('--bench-directory', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    source = args.report.absolute()
    original = json.loads(source.read_text())
    if original['status'] != 'failed' or len(original['courses']) != original['unique_content_sets']:
        p.error('Only a completed failed corpus can be followed up')
    if not original['failures'] or any(x['gate'] != 'paired_gpu' for x in original['failures']):
        p.error('Only paired GPU failures are eligible; host/historical failures need a new corpus run')
    if any(len(x['gpu']) != 3 or len(x['host']) != 2 for x in original['courses']):
        p.error('Incomplete original coverage')
    bench = args.bench_directory/'gt2renderbench'
    if digest(bench) != original['bench_sha256']:
        p.error('The exact original benchmark binary is required')
    out = Path(os.path.abspath(args.output))
    out.relative_to(ROOT/'work')
    if out == ROOT/'work': p.error('Use a new child directory under work')
    out.mkdir(parents=True, exist_ok=False)
    report = copy.deepcopy(original)
    report['status'] = 'running'
    report['review'] = dict(original_report=str(source), original_sha256=digest(source),
                            original_failures=original['failures'], repetitions=3, results=[])
    remote = '/data/local/tmp/gt2-sky-followup-'+uuid.uuid4().hex
    config = original['config']
    serial = original['serial']

    def save():
        (out/'report.json').write_text(json.dumps(report, indent=2)+'\n')

    def adb(*tail):
        code, text = command([args.adb, '-s', serial, *tail], timeout=180)
        if code: raise RuntimeError(text)
        return text

    save()
    try:
        if adb('shell', 'getprop', 'ro.product.model').strip() != original['device_model'].strip():
            raise RuntimeError('Device model differs from the original run')
        if adb('shell', 'getprop', 'ro.build.fingerprint').strip() != original['device_build'].strip():
            raise RuntimeError('Device software differs from the original run')
        adb('shell', 'mkdir', '-p', remote)
        adb('push', bench, remote+'/gt2renderbench')
        adb('push', args.bench_directory/'libopenxr_loader.so', remote+'/libopenxr_loader.so')
        adb('shell', 'chmod', '755', remote+'/gt2renderbench')
        remaining = []
        for failure in original['failures']:
            candidates = [(i, c) for i, c in enumerate(report['courses']) if c['course'] == failure['course']]
            if len(candidates) != 1: raise RuntimeError('Ambiguous failed course')
            index, course = candidates[0]
            chunk = failure['chunk']
            row = next(x for x in course['gpu'] if x['chunk'] == chunk)
            capture = retained_capture(source, index, course['course'], chunk)
            dest = out/f'{index:03d}-{course["course"]}-{chunk}'
            dest.mkdir()
            local = dest/'scene.gt2cap'
            with gzip.open(capture, 'rb') as stream, local.open('wb') as target:
                shutil.copyfileobj(stream, target)
            if digest(local) != row['capture_sha256']:
                raise RuntimeError('Retained capture checksum mismatch')
            adb('push', local, remote+'/scene.gt2cap')
            local.unlink()
            result = dict(course=course['course'], chunk=chunk, capture_sha256=row['capture_sha256'], runs=[])
            for repeat in range(3):
                before = adb('shell', 'dumpsys', 'battery')
                text = adb('shell', 'env', 'LD_LIBRARY_PATH='+remote, 'GT2_BENCH_MULTIVIEW=1', 'GT2_BENCH_SKY_PAIR=1',
                           remote+'/gt2renderbench', remote+'/scene.gt2cap', config['scale'], config['msaa'],
                           config['frames'], "''", 1, 0, config['foveation'])
                (dest/f'repeat-{repeat}.log').write_text(text)
                samples = paired_samples(text)
                measured = summarize(samples)
                measured.update(samples=samples, battery_before=before, battery_after=adb('shell', 'dumpsys', 'battery'))
                result['runs'].append(measured)
            result['passed'] = disposition(result['runs'])
            report['review']['results'].append(result)
            if result['passed']:
                row['original_measurement'] = copy.deepcopy(row)
                for key in ('before_ms', 'after_ms', 'before_p95_ms', 'after_p95_ms'):
                    row[key] = statistics.median(x[key] for x in result['runs'])
                row['saved_ms'] = row['before_ms']-row['after_ms']
                row['samples'] = [s for run in result['runs'] for s in run['samples']]
                row['followup'] = str(dest)
            else:
                remaining.append(failure)
            save()
        if digest(source) != report['review']['original_sha256']:
            raise RuntimeError('Original corpus report changed during follow-up')
        report['failures'] = remaining
        report['status'] = 'failed' if remaining else 'passed'
        save()
    except BaseException as error:
        report.update(status='interrupted', error=str(error));save();raise
    finally:
        adb('shell', 'rm', '-f', remote+'/scene.gt2cap', remote+'/gt2renderbench', remote+'/libopenxr_loader.so')
        adb('shell', 'rmdir', remote)
    print(json.dumps(dict(status=report['status'], cases=len(report['review']['results']), failures=report['failures'])))
    return int(report['status'] != 'passed')


if __name__ == '__main__':
    raise SystemExit(main())
