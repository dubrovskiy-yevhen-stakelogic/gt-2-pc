"""Build hash-bound native showroom poses from owned GT4 US 1.01 inputs.

The native exporter uses the recovered garage-car and showroom setup routines.
This wrapper resolves model labels from SpecDB and binds the result to every
decoded table consumed by build_gt4_wheels.py. It requires no separate checkout.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

TABLES = ('CAR_VARIATION_american', 'GENERIC_CAR', 'DEFAULT_PARTS', 'CHASSIS',
          'RACINGMODIFY', 'FRONTTIRE', 'REARTIRE', 'TIRESIZE', 'VARIATIONamerican')
CORE_ELF_SHA256 = '1b7dbe58b5437d7ea349feabfa61d033cb1cf6078c680dea4b931ac94143cf20'
# These model resources have no matching American car-variation record. Their
# native authored geometry remains usable without a showroom-pose override.
UNCONFIGURED_MODELS = {'niss0006', 'niss0014', 'rov10001', 'toyt0057', 'toyt0061'}


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('tool', 'core', 'specdb', 'specdb-json', 'alignment', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    if sha(args.core) != CORE_ELF_SHA256:
        raise ValueError('Showroom export requires the decompressed GT4 US 1.01 CORE image')
    tables = {}
    hashes = {}
    for name in TABLES:
        path = args.specdb_json / (name + '.json')
        hashes[name] = sha(path)
        tables[name] = json.loads(path.read_text(encoding='utf-8-sig'))['rows']
    variations = {row['id']: row['label'].rsplit('_', 1)[0]
                  for row in tables['VARIATIONamerican']}
    generic = {row['id']: row['label'] for row in tables['GENERIC_CAR']}
    model_labels = {}
    for row in tables['CAR_VARIATION_american']:
        variation = struct.unpack_from('<I', bytes.fromhex(row['hex']))[0]
        if variation in variations and row['id'] in generic:
            model_labels.setdefault(variations[variation], generic[row['id']])
    alignment = json.loads(args.alignment.read_text(encoding='utf-8-sig'))
    models = sorted({Path(row['source']).name for row in alignment})
    missing = set(models) - model_labels.keys()
    if missing - UNCONFIGURED_MODELS:
        raise ValueError('Unreviewed missing showroom models: ' + ', '.join(sorted(missing)))
    labels = sorted({model_labels[model] for model in models if model not in missing})
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='showroom-', dir=args.output.parent) as directory:
        directory = Path(directory)
        listing = directory / 'labels.txt'
        listing.write_text('\n'.join(labels) + '\n', encoding='ascii')
        native_path = directory / 'native.json'
        subprocess.run([str(args.tool.resolve()), str(args.core.resolve()),
                        str(args.specdb.resolve()), str(native_path.resolve()),
                        '@' + str(listing.resolve())], check=True)
        records = json.loads(native_path.read_text(encoding='utf-8-sig'))
        by_label = {record['label']: record for record in records}
        if len(records) != len(labels) or set(by_label) != set(labels):
            raise ValueError('Native showroom output has missing or duplicate labels')
        native_hash = sha(native_path)
    root = Path(__file__).resolve().parent.parent
    source_paths = [root / 'tools/gt4import/showroom_export.cpp', Path(__file__).resolve()]
    source_paths += sorted((root / 'tools/gt4import/native').rglob('*.cpp'))
    source_paths += sorted((root / 'tools/gt4import/native').rglob('*.h'))
    result = dict(
        format='gt4-showroom-geometry', version=1, specdbSources=hashes,
        models={model: by_label[model_labels[model]] for model in models if model not in missing},
        missingModels=sorted(missing),
        provenance=dict(
            coreELFSHA256=sha(args.core), builderExecutableSHA256=sha(args.tool),
            sourceFiles={path.relative_to(root).as_posix(): sha(path) for path in source_paths},
            specdbBinaries={path.name: sha(path) for path in sorted(args.specdb.glob('*.dbt'))},
            specdbLabels={path.name: sha(path) for path in sorted(args.specdb.glob('*.idi'))},
            coordinates='+X right,+Y up,-Z front; bodyMatrix and donorBasis column-major; wheel anchors body-local; donorBasis includes body rotation and excludes odd RY180',
            nativeResultsSHA256=native_hash))
    temporary = args.output.with_name(args.output.name + '.tmp')
    temporary.write_text(json.dumps(result, separators=(',', ':')) + '\n', encoding='utf-8')
    temporary.replace(args.output)
    print(f'Showroom poses: {len(result["models"])} models; no American configuration: {sorted(missing)}')


if __name__ == '__main__':
    main()
