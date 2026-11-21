"""Portable reader for GT2 MODE2/2352 discs and their GTFS assets."""
import hashlib
import mmap
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import zlib


PROFILES = {
    "231f9dba7191b9ef915621662afdc40a7c66df95": "arcade",
    "3030aa271c0a4022fc69ce09d76a6bc75e69a32a": "simulation",
    "2b59ad844a4dbb934fc1d8ef955c63038f54e932": "arcade",
    "5172a19c1d0fe07a2a61966653b755afe26d9e69": "simulation",
    "23803938bf67de78dd836839e690fd66c7f10d7e": "arcade",      # US Arcade v1.1 (the GT2+ Arcade base)
}
# Modified executables that keep a stock build's address layout (mirrors src/gt2formats/exe_profile.cpp).
MODS = {
    "9d733c40f91b7ff5c6b4e4141ce3ebf8f4f5bde1": "simulation",  # GT2+ Complete NTSC-U Simulation v1.2 beta 7
    "85aa2586fb8692a0e9830a1cd63ba6017a6f85d2": "arcade",      # GT2+ Complete NTSC-U Arcade v1.1 beta 7
}
# Combined discs patch their Simulation executable; the marker is the same as IsCombinedDisc.
COMBINED_BASES = {"SCUS_944.88", "SCES_123.80"}
MAX_ASSET = 256 * 1024 * 1024


def safe_relative(name):
    if (not name or "\\" in name or ":" in name or "\0" in name
            or any(p in ("", ".", "..") for p in name.split("/"))
            or PurePosixPath(name).is_absolute()):
        raise ValueError("Unsafe relative path: " + repr(name))
    return name


def digest(path, algorithm="sha256"):
    result = hashlib.new(algorithm)
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def resolve_image(path):
    path = Path(path).expanduser().resolve(strict=True)
    if path.suffix.lower() == ".cue":
        cue = path.read_text(encoding="utf-8-sig")
        files = re.findall(r'^\s*FILE\s+"([^"]+)"\s+BINARY\s*$', cue, re.M | re.I)
        tracks = re.findall(r'^\s*TRACK\s+(\d+)\s+(\S+)\s*$', cue, re.M | re.I)
        if len(files) != 1 or [(n, kind.upper()) for n, kind in tracks] != [("01", "MODE2/2352")]:
            raise ValueError("Use a single-track MODE2/2352 BIN/CUE disc.")
        safe_relative(files[0])
        path = (path.parent / files[0]).resolve(strict=True)
    if not path.is_file():
        raise ValueError("Not a disc image: " + str(path))
    return path


class Disc:
    def __init__(self, path):
        self.path = resolve_image(path)
        size = self.path.stat().st_size
        if not size or size % 2352:
            raise ValueError("A complete 2352-byte/sector BIN is required; 2048-byte ISO loses XA music/video.")
        self.stream = self.path.open("rb")
        self.raw = mmap.mmap(self.stream.fileno(), 0, access=mmap.ACCESS_READ)
        self.sectors = size // 2352
        try:
            self.root = self.read_iso()
            exe = next((name for name in self.root if name.startswith(("SCUS_", "SCES_", "SCPS_"))), None)
            if not exe:
                raise ValueError("Game executable not found.")
            lba, length = self.root[exe]
            if length > 4 * 1024 * 1024:
                raise ValueError("Invalid executable size.")
            data = self.form1(lba, 0, length)
            self.profile = hashlib.sha1(data).hexdigest()
            if self.profile in PROFILES or self.profile in MODS:
                self.mode = PROFILES.get(self.profile) or MODS[self.profile]
            elif exe in COMBINED_BASES and self.combined_marker():
                self.mode = "simulation"
            else:
                raise ValueError("Unsupported disc executable: " + exe + " (SHA-1 " + self.profile + ")")
            self.files = self.read_gtfs()
        except BaseException:
            self.close()
            raise

    def close(self):
        self.raw.close()
        self.stream.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def form1(self, lba, offset, size):
        start = lba * 2048 + offset
        if min(lba, offset, size) < 0 or start + size > self.sectors * 2048:
            raise ValueError("Disc read outside image.")
        sector, skip = divmod(start, 2048)
        chunks = []
        while size:
            count = min(size, 2048 - skip)
            at = sector * 2352 + 24 + skip
            chunks.append(self.raw[at:at + count])
            size -= count
            sector += 1
            skip = 0
        return b"".join(chunks)

    def read_iso(self):
        pvd = self.form1(16, 0, 2048)
        if pvd[:6] != b"\x01CD001":
            raise ValueError("ISO9660 descriptor not found in MODE2/2352 image.")
        lba = struct.unpack_from("<I", pvd, 158)[0]
        size = struct.unpack_from("<I", pvd, 166)[0]
        if pvd[156] < 34 or size > 16 * 1024 * 1024:
            raise ValueError("Invalid ISO root directory.")
        directory = self.form1(lba, 0, size)
        result, pos = {}, 0
        while pos < len(directory):
            length = directory[pos]
            if not length:
                pos = (pos // 2048 + 1) * 2048
                continue
            if length < 34 or pos + length > size or pos % 2048 + length > 2048:
                raise ValueError("Truncated ISO directory record.")
            rec = directory[pos:pos + length]
            if rec[32] + 33 > length:
                raise ValueError("Invalid ISO filename.")
            if not rec[25] & 2 and rec[32]:
                name = rec[33:33 + rec[32]].decode("ascii").split(";")[0]
                if name in result:
                    raise ValueError("Duplicate ISO filename.")
                result[name] = (struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0])
            pos += length
        return result

    def overlay_member(self, index):
        lba, length = self.root["GT2.OVL"]
        raw = self.form1(lba, 0, length)
        count = struct.unpack_from("<I", raw, 0)[0] // 8
        if index >= count:
            raise ValueError("GT2.OVL: member index out of range")
        offset, packed = struct.unpack_from("<II", raw, index * 8)
        return zlib.decompress(raw[offset:offset + packed], 31)

    def combined_marker(self):
        marker = bytes((0x07, 0x00, 0x06, 0x24, 0xFD, 0xFF, 0x02, 0x24))
        try:
            return marker in self.overlay_member(1)
        except Exception:
            return False

    def read_gtfs(self):
        if "GT2.VOL" not in self.root:
            raise ValueError("GT2.VOL not found.")
        self.vol_lba, vol_size = self.root["GT2.VOL"]
        header = self.form1(self.vol_lba, 0, 16)
        offsets, records = struct.unpack_from("<HH", header, 8)
        if header[:4] != b"GTFS" or offsets < 3:
            raise ValueError("Invalid GTFS header.")
        table = struct.unpack("<" + "I" * offsets, self.form1(self.vol_lba, 16, offsets * 4))
        starts = [n & ~2047 for n in table]
        sizes = [starts[i + 1] - starts[i] - (table[i] & 2047) for i in range(offsets - 1)]
        # A rebuilt VOL (GT2+) may place its last file at offsetCount-1 and let it run to the end of the volume.
        sizes.append(vol_size - starts[-1])
        if any(n < 0 for n in sizes) or any(n > vol_size for n in starts) or sizes[1] != records * 32:
            raise ValueError("Invalid GTFS offsets or TOC size.")
        toc = self.form1(self.vol_lba, starts[1], sizes[1])
        files, seen, visited = [], set(), set()

        def walk(first, prefix, depth):
            if depth > 16 or first in visited:
                raise ValueError("Cyclic or excessively nested GTFS directory.")
            visited.add(first)
            for i in range(first, records):
                rec = toc[i * 32:(i + 1) * 32]
                index, flags = struct.unpack_from("<HB", rec, 4)
                name = rec[7:].split(b"\0", 1)[0].decode("ascii")
                if name != "..":
                    safe_relative(name)
                    path = prefix + name
                    if flags & 1:
                        walk(index, path + "/", depth + 1)
                    elif 2 <= index < offsets:
                        if path in seen or sizes[index] > MAX_ASSET:
                            raise ValueError("Duplicate or oversized GTFS asset: " + path)
                        seen.add(path)
                        files.append((path, starts[index], sizes[index]))
                    else:
                        raise ValueError("Invalid GTFS file index.")
                if flags & 128:
                    return
            raise ValueError("Unterminated GTFS directory.")

        walk(0, "", 0)
        return files

    def asset(self, entry):
        _, offset, size = entry
        data = self.form1(self.vol_lba, offset, size)
        if data.startswith(b"\x1f\x8b"):
            # GtfsVolume::Read consumes one gzip member, even in a concatenated entry.
            stream = zlib.decompressobj(31)
            data = stream.decompress(data, MAX_ASSET + 1)
            if len(data) > MAX_ASSET:
                raise ValueError("Inflated asset exceeds size limit.")
            if not stream.eof:
                raise ValueError("Truncated compressed asset.")
        return data

    def extract(self, target):
        target = Path(target)
        target.mkdir()  # A new directory prevents overwriting saves or following existing symlinks.
        for number, entry in enumerate(self.files):
            path = target / "assets" / entry[0]
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(self.asset(entry))
            if number % 500 == 0:
                print(f"  {self.mode}: {number}/{len(self.files)} assets", flush=True)
        shutil.copyfile(self.path, target / "disc.raw2352")
        expected = digest(self.path)
        if digest(target / "disc.raw2352") != expected:
            raise ValueError("Disc copy failed SHA-256 verification.")
        return expected
