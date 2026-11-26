"""Synthetic GT4 volume checks; no game assets are used."""
import struct
from pathlib import Path
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from gt4_iso import inventory
from gt4_sources import for_toc


def directory_record(name, lba, size, flags=0):
    encoded = name.encode('ascii')
    length = 33 + len(encoded)
    length += length % 2
    record = bytearray(length)
    record[0] = length
    struct.pack_into('<I', record, 2, lba)
    struct.pack_into('>I', record, 6, lba)
    struct.pack_into('<I', record, 10, size)
    struct.pack_into('>I', record, 14, size)
    record[25] = flags
    record[28] = 1
    record[32] = len(encoded)
    record[33:33 + len(encoded)] = encoded
    return record


def synthetic_iso(path, count):
    root_lba, vol_lba = 20, 21
    pvd = bytearray(2048)
    pvd[:7] = b'\x01CD001\x01'
    pvd[40:72] = b'TESTVOL'.ljust(32)
    pvd[156:190] = directory_record('\x00', root_lba, 2048, 2)[:34]

    root = bytearray(2048)
    offset = vol_record = 0
    for record in (directory_record('\x00', root_lba, 2048, 2),
                   directory_record('\x01', root_lba, 2048, 2),
                   directory_record('GT4.VOL;1', vol_lba, 4 * 1024 * 1024)):
        if record[33:40] == b'GT4.VOL':
            vol_record = offset
        root[offset:offset + len(record)] = record
        offset += len(record)
    root[offset] = 0

    page = bytearray(2048)
    struct.pack_into('<HHii', page, 0, 0, count, -1, -1)
    page[12:19] = b'\x00\x00\x00\x00car'
    page[20] = 0
    struct.pack_into('<I', page, 21, 1)
    page[28:36] = b'\x00\x00\x00\x01test'
    page[36] = 1
    struct.pack_into('<I', page, 37, 0)
    struct.pack_into('<I', page, 45, 4)
    # Two real records plus the extra slot that GT4 Online-derived volumes keep.
    struct.pack_into('<HHHH', page, 2040, 28, 8, 36, 13)
    struct.pack_into('<HHHH', page, 2032, 12, 7, 20, 5)
    packer = zlib.compressobj(wbits=-15)
    packed = packer.compress(page) + packer.flush()

    offsets = struct.pack('<II', 72, 72 + len(packed))
    header = bytearray(64)
    header[:4] = b'RoFS'
    struct.pack_into('<HH', header, 4, 1, 3)
    struct.pack_into('<I', header, 8, 72 + len(packed))
    struct.pack_into('<I', header, 12, 1)
    struct.pack_into('<H', header, 16, 2048)
    struct.pack_into('<H', header, 18, 1)

    image = bytearray(vol_lba * 2048)
    image[16 * 2048:17 * 2048] = pvd
    image[root_lba * 2048:root_lba * 2048 + 2048] = root
    image += header + offsets + packed
    vol_size = max(2048, len(image) - vol_lba * 2048)
    image += b'\0' * (vol_lba * 2048 + vol_size - len(image))
    struct.pack_into('<I', image, root_lba * 2048 + vol_record + 10, vol_size)
    path.write_bytes(image)


class VolumeChecks(unittest.TestCase):
    def test_odd_entry_count_from_gt4_online_volumes(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'synthetic.iso'
            synthetic_iso(path, 5)
            with path.open('rb') as stream:
                result = inventory(stream)
        entries = {entry['path']: entry for entry in result['entries']}
        self.assertEqual(result['volume_label'], 'TESTVOL')
        self.assertEqual(entries['car']['node'], 1)
        self.assertEqual(entries['car/test']['parent'], 1)
        self.assertEqual(entries['car/test']['kind'], 1)

    def test_spec2_toc_is_a_supported_source(self):
        profiles = {'discTocSha256': 'retail', 'cars': {},
                    'discs': {'spec2': {'name': 'Spec II', 'specdb': 'GT4_PREMIUM_US2560',
                                        'catalogueCars': 744, 'core': False}}}
        self.assertEqual(for_toc(profiles, 'spec2')['catalogueCars'], 744)
        self.assertIsNone(for_toc(profiles, 'retail'))


if __name__ == '__main__':
    unittest.main()
