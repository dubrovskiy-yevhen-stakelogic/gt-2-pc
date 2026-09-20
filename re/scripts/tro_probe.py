"""Exploration probe for GT2 .tro track files (research script, not a parser).
usage: python tro_probe.py <file.tro> [chunk-index]
"""
import struct
import sys

d = open(sys.argv[1], 'rb').read()
ci = int(sys.argv[2]) if len(sys.argv) > 2 else 0


def u32(o): return struct.unpack_from('<I', d, o)[0]
def s32(o): return struct.unpack_from('<i', d, o)[0]
def u16(o): return struct.unpack_from('<H', d, o)[0]
def s16(o): return struct.unpack_from('<h', d, o)[0]


print('size', len(d), 'magic', d[:12], 'ver', hex(u32(12)))
hdr = [u32(0x10 + i * 4) for i in range(5)]
print('chunks/lodLookup/lodData/0x1C/0x20:', [hex(x) for x in hdr])
print('sectorCount', u32(0x24), 'instanced[0]', hex(u32(0x118)))
base = u32(0x118) - 0x19C
print('base', hex(base))

t = hdr[0] - base
num = s16(t + 4)
print('vcoordFinish', s16(t + 2), 'numChunks', num, 'unkCount', s16(t + 6))
offs = [u32(t + 12 + i * 4) - base for i in range(num)]
print('chunk offsets first 5:', [hex(x) for x in offs[:5]], 'spacing', [offs[i + 1] - offs[i] for i in range(4)])

c = offs[ci]
print(f'--- chunk {ci} @ {c:#x}: prev {u16(c)} next {u16(c + 2)}')
print('center', s32(c + 0x30), s32(c + 0x34), s32(c + 0x38))
shape = c + 0xA4
print('ptrs 0x94..0xA0', [hex(u32(c + 0x94 + i * 4)) for i in range(4)])


def shape_dump(s, title):
    o0 = u32(s) - base
    lists = [u32(s + 4 + i * 4) - base for i in range(8)]
    o24, o28 = u32(s + 0x24), u32(s + 0x28)
    cnt0 = s32(s + 0x2C)
    cnts = [s16(s + 0x30 + i * 2) for i in range(8)]
    cnt24 = s32(s + 0x40)
    print(f'{title} @ {s:#x}: off0 {o0:#x} count0 {cnt0} listCounts {cnts} o24 {o24:#x} o28 {o28:#x} cnt24 {cnt24}')
    print('  first entries of block 0 as 4 x s16:')
    for i in range(min(cnt0, 6)):
        print('   ', struct.unpack_from('<4h', d, o0 + i * 8))
    for li, (lo, n) in enumerate(zip(lists, cnts)):
        if n <= 0:
            continue
        stride = None
        nxt = sorted(x for x in lists + [o24 - base, o28 - base] if x > lo)
        if nxt:
            stride = (nxt[0] - lo) / n
        print(f'  list {li}: {n} entries @ {lo:#x}, bytes/entry to next block = {stride}')
        for i in range(min(n, 3)):
            print('    ', d[lo + i * 12: lo + i * 12 + 24].hex(' '))
    return o0, cnt0


shape_dump(shape, 'ChunkShape')
