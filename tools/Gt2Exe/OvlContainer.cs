using System.Buffers.Binary;
using System.IO.Compression;
using System.Text;

namespace Gt2Exe;

/// <summary>Parsed gzip member header (RFC 1952) plus trailer fields.</summary>
public sealed record GzipInfo(byte Method, byte Flags, uint MTime, byte Xfl, byte Os, string? FileName,
    int HeaderLength, uint TrailerCrc32, uint TrailerISize)
{
    public DateTime MTimeUtc => DateTimeOffset.FromUnixTimeSeconds(MTime).UtcDateTime;

    public static GzipInfo? TryParse(ReadOnlySpan<byte> d)
    {
        if (d.Length < 18 || d[0] != 0x1F || d[1] != 0x8B) return null;
        byte flags = d[3];
        int p = 10;
        if ((flags & 4) != 0) { int xlen = d[p] | d[p + 1] << 8; p += 2 + xlen; }
        string? name = null;
        if ((flags & 8) != 0)
        {
            int s = p;
            while (p < d.Length && d[p] != 0) p++;
            name = Encoding.ASCII.GetString(d[s..p]);
            p++;
        }
        if ((flags & 16) != 0) { while (p < d.Length && d[p] != 0) p++; p++; }
        if ((flags & 2) != 0) p += 2;
        return new GzipInfo(d[2], flags, BinaryPrimitives.ReadUInt32LittleEndian(d[4..]), d[8], d[9], name, p,
            BinaryPrimitives.ReadUInt32LittleEndian(d[^8..]), BinaryPrimitives.ReadUInt32LittleEndian(d[^4..]));
    }

    public static byte[] Decompress(ReadOnlySpan<byte> d)
    {
        using var input = new MemoryStream(d.ToArray(), writable: false);
        using var gz = new GZipStream(input, CompressionMode.Decompress);
        using var output = new MemoryStream();
        gz.CopyTo(output);
        return output.ToArray();
    }
}

public sealed record OvlEntry(int Index, uint Offset, uint PackedSize, GzipInfo? Gzip, byte[]? Unpacked);

/// <summary>
/// GT2.OVL container: a table of (u32 offset, u32 packedSize) pairs starting at file offset 0
/// (the table ends where the first member begins), followed by one gzip member per overlay.
/// </summary>
public static class OvlContainer
{
    public static List<OvlEntry> Parse(byte[] file)
    {
        var entries = new List<OvlEntry>();
        uint first = BinaryPrimitives.ReadUInt32LittleEndian(file);
        if (first == 0 || first % 8 != 0 || first > file.Length)
            throw new InvalidDataException($"Unexpected first offset 0x{first:X}.");
        int count = (int)(first / 8);
        for (int i = 0; i < count; i++)
        {
            uint off = BinaryPrimitives.ReadUInt32LittleEndian(file.AsSpan(i * 8));
            uint size = BinaryPrimitives.ReadUInt32LittleEndian(file.AsSpan(i * 8 + 4));
            if (off == 0 && size == 0) continue; // unused table slot
            if ((long)off + size > file.Length)
                throw new InvalidDataException($"Entry {i} exceeds file: off=0x{off:X} size=0x{size:X}.");
            var span = file.AsSpan((int)off, (int)size);
            var gz = GzipInfo.TryParse(span);
            byte[]? unpacked = gz != null ? GzipInfo.Decompress(span) : null;
            entries.Add(new OvlEntry(i, off, size, gz, unpacked));
        }
        return entries;
    }
}
