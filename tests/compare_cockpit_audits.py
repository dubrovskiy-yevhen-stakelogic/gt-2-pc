"""Compare every image in two local cockpit audit runs without changing images."""
import argparse
import hashlib
import html
import json
import os
from pathlib import Path

from PIL import Image, ImageChops


ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('baseline', type=Path)
    parser.add_argument('updated', type=Path)
    args = parser.parse_args()
    baseline, updated = args.baseline.resolve(), args.updated.resolve()
    for path in (baseline, updated):
        path.relative_to((ROOT / 'work').resolve())
    before = {path.name: path for path in (baseline / 'frames').glob('*.png')}
    after = {path.name: path for path in (updated / 'frames').glob('*.png')}
    if not before or before.keys() != after.keys():
        raise SystemExit('Audit image sets differ; compare complete runs over the same model corpus')
    changed = []
    hashes = []
    for name in sorted(before):
        old_hash, new_hash = digest(before[name]), digest(after[name])
        hashes.append(dict(image=name, baseline_sha256=old_hash, updated_sha256=new_hash))
        if old_hash == new_hash:
            continue
        with Image.open(before[name]) as old, Image.open(after[name]) as new:
            if old.size != new.size:
                raise SystemExit('Image dimensions differ: ' + name)
            difference = ImageChops.difference(old.convert('RGB'), new.convert('RGB'))
            red, green, blue = difference.split()
            maximum = ImageChops.lighter(ImageChops.lighter(red, green), blue)
            count = old.width * old.height - maximum.histogram()[0]
            changed.append(dict(image=name, pixels_changed=count,
                                fraction_changed=round(count / (old.width * old.height), 6),
                                bounds=difference.getbbox()))
    cars = sorted({row['image'].rsplit('-', 1)[0] for row in changed if row['pixels_changed']})
    result = dict(baseline=str(baseline), updated=str(updated), frames_compared=len(before),
                  baseline_summary=json.loads((baseline / 'summary.json').read_text(encoding='utf-8')),
                  updated_summary=json.loads((updated / 'summary.json').read_text(encoding='utf-8')),
                  byte_identical_frames=len(before)-len(changed), changed_cars=cars,
                  changed_frames=changed, hashes=hashes)
    (updated / 'comparison.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    (updated / 'changed-cars.txt').write_text('\n'.join(cars) + '\n', encoding='utf-8')
    rows = []
    for change in changed:
        name = change['image']
        images = ''.join('<td><img src="' + html.escape(Path(os.path.relpath(path, updated)).as_posix(), quote=True) + '"></td>'
                         for path in (before[name], after[name]))
        rows.append('<tr><th>' + html.escape(name) + '<br>' + str(change['pixels_changed']) + ' pixels</th>' + images + '</tr>')
    (updated / 'comparison.html').write_text(
        '<!doctype html><meta charset="utf-8"><title>Cockpit audit comparison</title>'
        '<style>body{font:16px system-ui;background:#eee}table{border-collapse:collapse}'
        'td,th{padding:8px;border:1px solid #aaa}img{width:320px;height:320px}</style>'
        '<h1>Cockpit audit comparison</h1><p>All images compared: ' + str(len(before)) +
        '. Changed cars: ' + str(len(cars)) + '.</p><table><tr><th>Image</th><th>Baseline</th><th>Updated</th></tr>' +
        ''.join(rows) + '</table>', encoding='utf-8')
    print(f'{len(before)} frames compared: {len(changed)} changed, {len(cars)} affected cars; ' + str(updated / 'comparison.html'))


if __name__ == '__main__':
    main()
