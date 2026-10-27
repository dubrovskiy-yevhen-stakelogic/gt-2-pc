"""Unpack CORE.GT4 (GT4 US 1.01 main executable image) into a loadable ELF for analysis.

Container layout, derived from our disassembly of the loader SCUS_973.28
(SHA-1 1df6f08c348e55f66c3a38e1774a9a989ce7d6ba): reader FUN_01004308, verifier FUN_01004500/FUN_01004448,
segment copier FUN_010047c0.

  file:   u8 1, u8 1, u32le inflated_size, raw DEFLATE stream
  inflated:
          u16le len_a, u8[len_a]      signature block (verified by FUN_01004500)
          u16le len_b, u8[len_b]      second verifier block
          image: u32le segment_count, u32le entry, then segment_count x {u32le vaddr, u32le size, u8[size]}
  The image is hashed with SHA-512 (FUN_01004448) for the signature check; it is not encrypted.

Output stays under work/ (game code). No game bytes belong in the repository.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

KNOWN_CORE_SHA1 = '30031137ce494323816bb707b90d3ec0d07937d2'


def unpack(data):
    if data[0] != 1 or data[1] != 1:
        raise ValueError(f'Unexpected CORE header {data[:2].hex()}')
    size = struct.unpack_from('<I', data, 2)[0]
    inflater = zlib.decompressobj(-15)
    raw = inflater.decompress(data[6:], size + 1)
    if len(raw) != size or inflater.unconsumed_tail or inflater.unused_data:
        raise ValueError('Inflated size mismatch')
    len_a = struct.unpack_from('<H', raw, 0)[0]
    len_b = struct.unpack_from('<H', raw, 2 + len_a)[0]
    image = raw[4 + len_a + len_b:]
    count, entry = struct.unpack_from('<II', image, 0)
    pos, segments = 8, []
    for _ in range(count):
        vaddr, length = struct.unpack_from('<II', image, pos)
        if pos + 8 + length > len(image):
            raise ValueError('Segment exceeds image')
        segments.append((vaddr, image[pos + 8:pos + 8 + length]))
        pos += 8 + length
    if pos != len(image):
        raise ValueError(f'{len(image) - pos} trailing bytes after segments')
    return dict(entry=entry, segments=segments, block_a=raw[2:2 + len_a], block_b=raw[4 + len_a:4 + len_a + len_b],
                image_sha512=hashlib.sha512(image).hexdigest())


def build_elf(entry, segments):
    """Minimal ELF32 LE MIPS executable, one PT_LOAD per segment (R5900 flags as in the PS2 SDK)."""
    segments = sorted(segments)
    phoff, phnum = 0x34, len(segments)
    offset = (phoff + 32 * phnum + 0xfff) & ~0xfff
    headers, body = b'', b''
    for vaddr, blob in segments:
        pad = (-(offset + len(body)) % 16)
        body += b'\0' * pad
        # The 0x18-byte segment is the MIPS .reginfo (gp in its last word); in this build it overlays the last
        # bytes of the text segment with identical content, so it is exposed as PT_MIPS_REGINFO, not loaded twice.
        kind = 0x70000000 if len(blob) == 0x18 else 1
        headers += struct.pack('<8I', kind, offset + len(body), vaddr, vaddr, len(blob), len(blob),
                               4 if kind != 1 else 7, 4 if kind != 1 else 16)
        body += blob
    ident = b'\x7fELF\x01\x01\x01' + b'\0' * 9
    ehdr = ident + struct.pack('<HHIIIIIHHHHHH', 2, 8, 1, entry, phoff, 0, 0x20924001, 0x34, 32, phnum, 40, 0, 0)
    head = ehdr + headers
    return head + b'\0' * (offset - len(head)) + body


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('core', type=Path, help='CORE.GT4 extracted from the disc')
    parser.add_argument('--output', type=Path, required=True, help='Directory under work/')
    args = parser.parse_args()
    data = args.core.read_bytes()
    sha1 = hashlib.sha1(data).hexdigest()
    result = unpack(data)
    args.output.mkdir(parents=True, exist_ok=True)
    elf = build_elf(result['entry'], result['segments'])
    (args.output / 'CORE.elf').write_bytes(elf)
    for vaddr, blob in result['segments']:
        (args.output / f'seg_{vaddr:08x}.bin').write_bytes(blob)
    report = dict(core_sha1=sha1, known_build=sha1 == KNOWN_CORE_SHA1, entry=f'{result["entry"]:08x}',
                  image_sha512=result['image_sha512'], elf_sha1=hashlib.sha1(elf).hexdigest(),
                  segments=[dict(vaddr=f'{v:08x}', size=len(b), sha1=hashlib.sha1(b).hexdigest())
                            for v, b in result['segments']])
    (args.output / 'core.json').write_text(json.dumps(report, indent=2), encoding='utf8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
