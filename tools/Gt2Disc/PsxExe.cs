using System.Buffers.Binary;
using System.Text;

namespace Gt2Disc;

/// <summary>Header of a PlayStation "PS-X EXE" executable (first 0x800 bytes of the file).</summary>
public sealed class PsxExeHeader
{
    public const int HeaderSize = 0x800;

    public required string Magic { get; init; }
    public required uint Pc0 { get; init; }
    public required uint Gp0 { get; init; }
    public required uint TAddr { get; init; }
    public required uint TSize { get; init; }
    public required uint DAddr { get; init; }
    public required uint DSize { get; init; }
    public required uint BAddr { get; init; }
    public required uint BSize { get; init; }
    public required uint SAddr { get; init; }
    public required uint SSize { get; init; }
    public required string RegionMarker { get; init; }

    public bool IsValid => Magic == "PS-X EXE";

    public static PsxExeHeader Parse(ReadOnlySpan<byte> header)
    {
        if (header.Length < 0x100) throw new ArgumentException("Need at least 0x100 header bytes.");
        uint U32(int o, ReadOnlySpan<byte> h) => BinaryPrimitives.ReadUInt32LittleEndian(h[o..]);

        int end = 0x4C;
        while (end < header.Length && end < HeaderSize && header[end] != 0) end++;

        return new PsxExeHeader
        {
            Magic = Encoding.ASCII.GetString(header[..8]),
            Pc0 = U32(0x10, header),
            Gp0 = U32(0x14, header),
            TAddr = U32(0x18, header),
            TSize = U32(0x1C, header),
            DAddr = U32(0x20, header),
            DSize = U32(0x24, header),
            BAddr = U32(0x28, header),
            BSize = U32(0x2C, header),
            SAddr = U32(0x30, header),
            SSize = U32(0x34, header),
            RegionMarker = Encoding.ASCII.GetString(header[0x4C..end]),
        };
    }

    public void Print(TextWriter w, long fileSize)
    {
        w.WriteLine($"  magic        : {Magic}");
        w.WriteLine($"  pc0          : 0x{Pc0:X8}");
        w.WriteLine($"  gp0          : 0x{Gp0:X8}");
        w.WriteLine($"  t_addr       : 0x{TAddr:X8}");
        w.WriteLine($"  t_size       : 0x{TSize:X8} ({TSize})  -> text end 0x{TAddr + TSize:X8}");
        w.WriteLine($"  d_addr/size  : 0x{DAddr:X8} / 0x{DSize:X8}");
        w.WriteLine($"  b_addr/size  : 0x{BAddr:X8} / 0x{BSize:X8}");
        w.WriteLine($"  s_addr/size  : 0x{SAddr:X8} / 0x{SSize:X8}");
        w.WriteLine($"  region marker: \"{RegionMarker}\"");
        w.WriteLine($"  file size    : {fileSize} (header 0x800 + t_size = {HeaderSize + (long)TSize})");
    }
}
