using System.Buffers.Binary;
using System.Text;

namespace Gt2Vol;

/// <summary>
/// Read-only parser for the Gran Turismo 2 "GTFS" archive (GT2.VOL).
///
/// Layout (all little-endian, verified on SCUS-94455 v1.1 and SCUS-94488 v1.2):
///   0x00  char[4]  "GTFS"
///   0x04  u32      0
///   0x08  u16      offsetCount   number of entries in the offset table
///   0x0A  u16      recordCount   number of 32-byte TOC records
///   0x0C  u32      0
///   0x10  u32[offsetCount] offset table
///
/// Offset table entry: bits 31..11 = index of the 2048-byte block where the
/// entry starts, bits 10..0 = number of padding bytes at the end of the entry's
/// last block. size[i] = start[i+1] - start[i] - pad[i].
///   entry 0 = the header + offset table itself
///   entry 1 = the TOC (recordCount * 32 bytes)
///   entry 2.. = file data; the final entry is an end-of-data sentinel.
///
/// TOC record (32 bytes):
///   0x00 u32  unix timestamp
///   0x04 u16  file: index into the offset table; directory: index of the
///             directory's first TOC record
///   0x06 u8   flags: 0x01 = directory, 0x80 = last record of this directory
///   0x07 char[25] name, NUL padded
/// </summary>
public sealed class GtfsVolume : IDisposable
{
    public const int BlockSize = 2048;
    public const int RecordSize = 32;
    public const int HeaderSize = 0x10;

    private readonly FileStream _stream;

    public string Path { get; }
    public long Length { get; }
    public int OffsetCount { get; }
    public int RecordCount { get; }
    public uint[] RawOffsets { get; }
    public IReadOnlyList<TocRecord> Records { get; }
    public IReadOnlyList<VolEntry> Entries { get; }
    public IReadOnlyList<string> Warnings => _warnings;

    private readonly List<string> _warnings = new();

    public GtfsVolume(string path)
    {
        Path = path;
        _stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16, FileOptions.RandomAccess);
        Length = _stream.Length;

        Span<byte> header = stackalloc byte[HeaderSize];
        ReadExactAt(0, header);
        if (header[0] != (byte)'G' || header[1] != (byte)'T' || header[2] != (byte)'F' || header[3] != (byte)'S')
            throw new InvalidDataException("Not a GTFS volume (bad magic).");

        OffsetCount = BinaryPrimitives.ReadUInt16LittleEndian(header[0x08..]);
        RecordCount = BinaryPrimitives.ReadUInt16LittleEndian(header[0x0A..]);

        var table = new byte[OffsetCount * 4];
        ReadExactAt(HeaderSize, table);
        RawOffsets = new uint[OffsetCount];
        for (int i = 0; i < OffsetCount; i++)
            RawOffsets[i] = BinaryPrimitives.ReadUInt32LittleEndian(table.AsSpan(i * 4));

        var toc = new byte[GetSize(1)];
        ReadExactAt(GetStart(1), toc);
        if (toc.Length != RecordCount * RecordSize)
            _warnings.Add($"TOC size {toc.Length} != recordCount*32 = {RecordCount * RecordSize}");

        int usable = Math.Min(RecordCount, toc.Length / RecordSize);
        var records = new List<TocRecord>(usable);
        for (int i = 0; i < usable; i++)
        {
            var r = toc.AsSpan(i * RecordSize, RecordSize);
            int nameLen = r[7..].IndexOf((byte)0);
            if (nameLen < 0) nameLen = 25;
            records.Add(new TocRecord(
                i,
                BinaryPrimitives.ReadUInt32LittleEndian(r),
                BinaryPrimitives.ReadUInt16LittleEndian(r[4..]),
                r[6],
                Encoding.ASCII.GetString(r.Slice(7, nameLen))));
        }
        Records = records;
        Entries = BuildTree(records);
    }

    public long GetStart(int offsetIndex) => (long)(RawOffsets[offsetIndex] >> 11) * BlockSize;

    public int GetPadding(int offsetIndex) => (int)(RawOffsets[offsetIndex] & 0x7FF);

    /// <summary>Exact byte size of an offset-table entry (the sentinel has no size).</summary>
    public long GetSize(int offsetIndex)
    {
        if (offsetIndex + 1 >= OffsetCount)
            return 0;
        return GetStart(offsetIndex + 1) - GetStart(offsetIndex) - GetPadding(offsetIndex);
    }

    public byte[] ReadEntry(int offsetIndex)
    {
        var data = new byte[GetSize(offsetIndex)];
        ReadExactAt(GetStart(offsetIndex), data);
        return data;
    }

    public void ReadExactAt(long position, Span<byte> buffer)
    {
        _stream.Position = position;
        _stream.ReadExactly(buffer);
    }

    private List<VolEntry> BuildTree(List<TocRecord> records)
    {
        var result = new List<VolEntry>();
        var visited = new bool[records.Count];
        Walk(0, "", 0);

        int orphan = visited.Count(v => !v);
        if (orphan != 0)
            _warnings.Add($"{orphan} TOC records are not reachable from the root directory");
        return result;

        void Walk(int first, string prefix, int depth)
        {
            if (depth > 16)
            {
                _warnings.Add($"directory nesting too deep at record {first}");
                return;
            }
            for (int i = first; i < records.Count; i++)
            {
                var rec = records[i];
                if (visited[i])
                {
                    _warnings.Add($"record {i} visited twice");
                    return;
                }
                visited[i] = true;

                if (rec.Name != "..")
                {
                    string full = prefix + rec.Name;
                    if (rec.IsDirectory)
                    {
                        result.Add(new VolEntry(full, true, rec, 0, 0));
                        Walk(rec.Index, full + "/", depth + 1);
                    }
                    else if (rec.Index < 2 || rec.Index >= OffsetCount - 1)
                    {
                        _warnings.Add($"record {i} '{full}' has offset index {rec.Index} out of range");
                    }
                    else
                    {
                        result.Add(new VolEntry(full, false, rec, GetStart(rec.Index), GetSize(rec.Index)));
                    }
                }
                if (rec.IsLast)
                    return;
            }
            _warnings.Add($"directory starting at record {first} has no terminating record");
        }
    }

    public void Dispose() => _stream.Dispose();
}

public sealed record TocRecord(int RecordIndex, uint Timestamp, ushort Index, byte Flags, string Name)
{
    public bool IsDirectory => (Flags & 0x01) != 0;
    public bool IsLast => (Flags & 0x80) != 0;
    public DateTime TimeUtc => DateTime.UnixEpoch.AddSeconds(Timestamp);
}

public sealed record VolEntry(string FullPath, bool IsDirectory, TocRecord Record, long Offset, long Size);
