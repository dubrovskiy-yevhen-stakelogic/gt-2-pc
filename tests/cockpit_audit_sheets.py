"""Index a headless cockpit audit into contact sheets; local game art stays in work/."""
import argparse
import csv
import json
from pathlib import Path
from PIL import Image, ImageChops, ImageDraw


def opening_fraction(image):
    """Measure visible diagnostic backdrop; useful for triage, not a pass/fail oracle."""
    count = 0
    for colour in ((26, 179, 230), (217, 163, 31)):
        channels = ImageChops.difference(image, Image.new('RGB', image.size, colour)).split()
        maximum = ImageChops.lighter(ImageChops.lighter(channels[0], channels[1]), channels[2])
        count += sum(maximum.histogram()[:5])
    return round(count / (image.width * image.height), 6)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('audit', type=Path)
    parser.add_argument('--changed-only', action='store_true', help='Use changed_cars from this audit comparison.json')
    args = parser.parse_args()
    audit = args.audit.resolve()
    audit.relative_to((Path.cwd() / 'work').resolve())
    with (audit / 'audit.csv').open(encoding='utf-8') as report:
        rows = list(csv.DictReader(report))
    cars = sorted(row['car'] for row in rows if row['status'] == 'ok')
    if args.changed_only:
        changed = set(json.loads((audit / 'comparison.json').read_text(encoding='utf-8'))['changed_cars'])
        if changed - set(cars):
            parser.error('Comparison names cars missing from this audit')
        cars = [car for car in cars if car in changed]
    suffix = '-changed' if args.changed_only else ''
    sheets = audit / ('sheets' + suffix)
    sheets.mkdir(exist_ok=True)
    angles = ('front', 'left', 'right', 'seat')
    manifest = []
    coverage = []
    # Each car is one four-angle row, grouped in two columns. 16 cars per sheet.
    for page, start in enumerate(range(0, len(cars), 16), 1):
        group = cars[start:start + 16]
        image = Image.new('RGB', (1600, 8 * 222), '#f4f4f4')
        draw = ImageDraw.Draw(image)
        for slot, car in enumerate(group):
            x = (slot // 8) * 800
            y = (slot % 8) * 222
            draw.text((x + 3, y + 3), car + '     FRONT                  LEFT                   RIGHT                   SEAT', fill='black')
            fractions = {'car': car}
            for angle_index, angle in enumerate(angles):
                frame = Image.open(audit / 'frames' / f'{car}-{angle}.png').convert('RGB')
                fractions[angle] = opening_fraction(frame)
                frame.thumbnail((194, 194), Image.Resampling.LANCZOS)
                image.paste(frame, (x + angle_index * 200 + 3, y + 23))
            coverage.append(fractions)
        name = f'{page:03d}.jpg'
        image.save(sheets / name, quality=92)
        manifest.append({'sheet': name, 'cars': group})
    (audit / ('sheet-index' + suffix + '.json')).write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    with (audit / ('opening-coverage' + suffix + '.csv')).open('w', newline='', encoding='utf-8') as report:
        writer = csv.DictWriter(report, fieldnames=('car', *angles))
        writer.writeheader()
        writer.writerows(coverage)
    print(f'{len(cars)} cars, {len(manifest)} contact sheets at {sheets}')


if __name__ == '__main__':
    main()
