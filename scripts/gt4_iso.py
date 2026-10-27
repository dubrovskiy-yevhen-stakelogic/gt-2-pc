"""Read-only GT4 ISO inventory and optional raw car extraction (not a mesh converter).

Format reference: https://nenkai.github.io/gt-modding-hub/formats/volume/ps2_gt4_volume/
Research outputs belong under work/; no game assets are included with this script.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import struct
import zlib


def u32(data, offset=0):
    return struct.unpack_from('<I', data, offset)[0]


def read_at(stream, offset, size):
    if offset < 0 or size < 0:
        raise ValueError('Negative read range')
    stream.seek(offset)
    data = stream.read(size)
    if len(data) != size:
        raise ValueError(f'Truncated input at {offset}: {size} requested, {len(data)} read')
    return data


def inventory(stream):
    pvd = read_at(stream, 16 * 2048, 2048)
    if pvd[:7] != b'\x01CD001\x01':
        raise ValueError('Expected ISO9660 with 2048-byte sectors')
    root = pvd[156:190]
    directory = read_at(stream, u32(root, 2) * 2048, u32(root, 10))
    files = []
    pos = 0
    while pos < len(directory):
        length = directory[pos]
        if length == 0:
            pos = (pos // 2048 + 1) * 2048
            continue
        if length < 34 or pos + length > len(directory):
            raise ValueError('Malformed ISO directory record')
        record = directory[pos:pos + length]
        name = record[33:33 + record[32]].decode('ascii').split(';')[0]
        if name not in ('\x00', '\x01'):
            files.append(dict(name=name, lba=u32(record, 2), size=u32(record, 10)))
        pos += length
    vol = next(x for x in files if x['name'] == 'GT4.VOL')
    vol_start = vol['lba'] * 2048
    header = None
    for sector in range(min(10000, vol['size'] // 2048)):
        candidate = read_at(stream, vol_start + sector * 2048, 20)
        if candidate[:4] in (b'RoFS', bytes.fromhex('ad90b9ac')) and u32(candidate, 4) == 0x30001:
            header = candidate
            break
    if header is None:
        raise ValueError('No supported RoFS 3.1 header')
    base = vol_start + sector * 2048
    compressed_length, page_count, page_size, toc_count = struct.unpack_from('<IIHH', header, 8)
    if page_size != 2048 or not 0 < toc_count < 20000:
        raise ValueError('Invalid RoFS page layout')
    encrypted = header[:4] != b'RoFS'
    raw_offsets = read_at(stream, base + 64, 4 * (toc_count + 1))
    offsets = [u32(raw_offsets, i * 4) ^ (((i + 1) * 0x14ac327a) & 0xffffffff if encrypted else 0)
               for i in range(toc_count + 1)]
    if offsets[0] < 64 + len(raw_offsets) or offsets[-1] != compressed_length:
        raise ValueError('Invalid RoFS table bounds')
    entries = []
    toc_digest = hashlib.sha256(header + raw_offsets)
    for begin, end in zip(offsets, offsets[1:]):
        if not 0 < end - begin <= 65536:
            raise ValueError('Invalid compressed page length')
        packed = read_at(stream, base + begin, end - begin)
        toc_digest.update(packed)
        # Original pages can omit the final DEFLATE marker. Exact output size
        # and every record range are checked independently below.
        inflater = zlib.decompressobj(-15)
        page = inflater.decompress(packed, page_size + 1)
        if len(page) != page_size or inflater.unconsumed_tail:
            raise ValueError('Invalid inflated page size')
        if encrypted:
            page = bytes(x ^ 0x55 for x in page)
        index_page, count = struct.unpack_from('<HH', page)
        if index_page:
            continue
        if count % 2 or count * 4 > page_size - 12:
            raise ValueError('Invalid entry count')
        for index in range(count // 2):
            no, nl, mo, ml = struct.unpack_from('<HHHH', page, page_size - 8 * (index + 1))
            if nl < 4 or ml < 5 or no + nl > page_size or mo + ml > page_size:
                raise ValueError('Invalid entry bounds')
            meta = page[mo:mo + ml]
            entry = dict(parent=struct.unpack_from('>I', page, no)[0],
                         name=page[no + 4:no + nl].decode('utf8'), kind=meta[0])
            if meta[0] == 0:
                entry['node'] = u32(meta, 1)
            elif meta[0] in (1, 2):
                if ml < (17 if meta[0] == 2 else 13):
                    raise ValueError('Truncated file record')
                entry.update(offset=base + page_count * page_size + u32(meta, 1) * page_size,
                             stored_size=u32(meta, 9), size=u32(meta, 13) if meta[0] == 2 else u32(meta, 9))
            else:
                raise ValueError('Unsupported record type')
            entries.append(entry)
    dirs = {entry['node']: entry for entry in entries if entry['kind'] == 0}
    for entry in entries:
        parts = [entry['name']]
        parent = entry['parent']
        visited = set()
        while parent:
            if parent in visited:
                raise ValueError('Directory cycle')
            visited.add(parent)
            directory = dirs[parent]
            parts.append(directory['name'])
            parent = directory['parent']
        entry['path'] = '/'.join(x for x in reversed(parts) if x not in ('', '.', '/'))
    return dict(volume_label=pvd[40:72].decode('ascii').strip(), iso_root=files,
                rofs_offset=base, toc_fingerprint_sha256=toc_digest.hexdigest(),
                entries=entries)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('iso', type=Path)
    parser.add_argument('--output', type=Path, required=True, help='New research output directory')
    parser.add_argument('--extract-car', action='append', default=[], help='Exact raw GT4 model code, repeatable')
    args = parser.parse_args()
    if args.output.exists():
        parser.error('Output must be a new directory; existing research is never overwritten')
    args.output.mkdir(parents=True)
    with args.iso.open('rb') as stream:
        report = inventory(stream)
        report.update(iso=str(args.iso.resolve()), iso_size=args.iso.stat().st_size)
        files = [x for x in report['entries'] if x['kind']]
        counts = collections.Counter('/'.join(x['path'].split('/')[:2]) for x in files)
        report['file_counts'] = dict(sorted(counts.items()))
        report['extracted'] = []
        for entry in files:
            path = entry['path']
            if not path.startswith(('car/lod/', 'car/menu/', 'car/open/')):
                continue
            if entry['name'].removesuffix('.pat') not in args.extract_car:
                continue
            destination = (args.output / 'raw' / path).resolve()
            if not destination.is_relative_to(args.output.resolve() / 'raw'):
                raise ValueError('Unsafe archive path')
            if entry['stored_size'] > 64 * 1024 * 1024 or entry['size'] > 64 * 1024 * 1024:
                raise ValueError('Unexpected model file size')
            data = read_at(stream, entry['offset'], entry['stored_size'])
            if entry['kind'] == 2:
                if data[:4] != bytes.fromhex('c5eef7ff'):
                    raise ValueError(f'Unrecognized compressed header: {path}: {data[:8].hex()}')
                if ((-u32(data, 4)) & 0xffffffff) != entry['size']:
                    raise ValueError('Compression header size mismatch')
                inflater = zlib.decompressobj(-15)
                data = inflater.decompress(data[8:], entry['size'] + 1)
                if inflater.unconsumed_tail:
                    raise ValueError('Expanded model exceeds size limit')
            if len(data) != entry['size']:
                raise ValueError(f'Extracted size mismatch: {path}')
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
            report['extracted'].append(dict(path=path, size=len(data), magic=data[:4].hex(), sha256=hashlib.sha256(data).hexdigest()))
        (args.output / 'inventory.json').write_text(json.dumps(report, indent=2), encoding='utf8')
        print(json.dumps(dict(output=str(args.output.resolve()), files=len(files),
                              cars={k:v for k,v in counts.items() if k.startswith('car/')},
                              extracted=len(report['extracted'])), indent=2))


if __name__ == '__main__':
    main()
