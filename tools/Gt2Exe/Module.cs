using System.Buffers.Binary;
using System.Text;

namespace Gt2Exe;

/// <summary>
/// A loaded code module: either a PS-X EXE (header stripped, base = t_addr) or a raw blob
/// (overlay) with a caller-supplied load address. All offsets are relative to <see cref="Data"/>.
/// </summary>
public sealed class Module
{
    public const int ExeHeaderSize = 0x800;

    public required string Path { get; init; }
    public required string Name { get; init; }
    public required byte[] Data { get; init; }
    public required uint Base { get; init; }
    /// <summary>Bytes skipped at the start of the file (0x800 for PS-X EXE).</summary>
    public required int FileSkip { get; init; }
    public PsxExeHeader? Header { get; init; }

    public uint End => Base + (uint)Data.Length;
    public bool Contains(uint addr) => addr >= Base && addr < End;
    public uint Word(int offset) => BinaryPrimitives.ReadUInt32LittleEndian(Data.AsSpan(offset));
    public uint AddrOf(int offset) => Base + (uint)offset;

    public static Module Load(string path, uint? baseOverride = null)
    {
        byte[] file = File.ReadAllBytes(path);
        string name = System.IO.Path.GetFileName(path);
        if (file.Length >= ExeHeaderSize && Encoding.ASCII.GetString(file, 0, 8) == "PS-X EXE")
        {
            var hdr = PsxExeHeader.Parse(file.AsSpan(0, ExeHeaderSize));
            return new Module
            {
                Path = path, Name = name, Header = hdr, FileSkip = ExeHeaderSize,
                Data = file.AsSpan(ExeHeaderSize).ToArray(),
                Base = baseOverride ?? hdr.TAddr,
            };
        }
        return new Module
        {
            Path = path, Name = name, FileSkip = 0, Data = file,
            Base = baseOverride ?? 0x80010000u,
        };
    }
}

/// <summary>Header of a PlayStation "PS-X EXE" executable (first 0x800 bytes of the file).</summary>
public sealed class PsxExeHeader
{
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
    public required uint[] RawWords { get; init; }
    public required string RegionMarker { get; init; }

    public static PsxExeHeader Parse(ReadOnlySpan<byte> h)
    {
        uint U32(ReadOnlySpan<byte> s, int o) => BinaryPrimitives.ReadUInt32LittleEndian(s[o..]);
        int end = 0x4C;
        while (end < h.Length && h[end] != 0) end++;
        var raw = new uint[0x4C / 4];
        for (int i = 0; i < raw.Length; i++) raw[i] = U32(h, i * 4);
        return new PsxExeHeader
        {
            Magic = Encoding.ASCII.GetString(h[..8]),
            Pc0 = U32(h, 0x10), Gp0 = U32(h, 0x14),
            TAddr = U32(h, 0x18), TSize = U32(h, 0x1C),
            DAddr = U32(h, 0x20), DSize = U32(h, 0x24),
            BAddr = U32(h, 0x28), BSize = U32(h, 0x2C),
            SAddr = U32(h, 0x30), SSize = U32(h, 0x34),
            RawWords = raw,
            RegionMarker = Encoding.ASCII.GetString(h[0x4C..end]),
        };
    }

    public void Print(TextWriter w, long fileSize)
    {
        w.WriteLine($"  magic         : {Magic}");
        w.WriteLine($"  pc0           : 0x{Pc0:X8}");
        w.WriteLine($"  gp0           : 0x{Gp0:X8}");
        w.WriteLine($"  t_addr/t_size : 0x{TAddr:X8} / 0x{TSize:X} ({TSize}) -> image end 0x{TAddr + TSize:X8}");
        w.WriteLine($"  d_addr/d_size : 0x{DAddr:X8} / 0x{DSize:X} -> 0x{DAddr + DSize:X8}");
        w.WriteLine($"  b_addr/b_size : 0x{BAddr:X8} / 0x{BSize:X}");
        w.WriteLine($"  s_addr/s_size : 0x{SAddr:X8} / 0x{SSize:X}");
        w.WriteLine($"  region marker : \"{RegionMarker}\"");
        w.WriteLine($"  file size     : {fileSize} (0x800 + t_size = {0x800 + (long)TSize})");
        w.Write("  raw words     :");
        for (int i = 2; i < RawWords.Length; i++) w.Write($" {RawWords[i]:X8}");
        w.WriteLine();
    }
}
