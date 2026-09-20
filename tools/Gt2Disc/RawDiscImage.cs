namespace Gt2Disc;

/// <summary>
/// CD-ROM XA subheader submode bits (byte 18 of a raw Mode 2 sector).
/// </summary>
[Flags]
public enum Submode : byte
{
    None = 0,
    EndOfRecord = 0x01,
    Video = 0x02,
    Audio = 0x04,
    Data = 0x08,
    Trigger = 0x10,
    Form2 = 0x20,
    RealTime = 0x40,
    EndOfFile = 0x80,
}

/// <summary>
/// Decoded header + XA subheader of one raw 2352-byte sector.
/// </summary>
public readonly record struct SectorHeader(byte Minute, byte Second, byte Frame, byte Mode,
    byte FileNumber, byte Channel, Submode Submode, byte CodingInfo)
{
    public bool IsForm2 => Mode == 2 && (Submode & Submode.Form2) != 0;
}

/// <summary>
/// Read-only access to a raw (2352 bytes per sector) single-track CD image.
/// The image file is opened read-only and is never modified.
/// </summary>
public sealed class RawDiscImage : IDisposable
{
    public const int RawSectorSize = 2352;
    public const int SubheaderOffset = 16;
    public const int UserDataOffset = 24;   // Mode 2 (Form 1 and Form 2)
    public const int Form1DataSize = 2048;
    public const int Form2DataSize = 2324;

    private static readonly byte[] SyncPattern =
        { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };

    private readonly FileStream _stream;

    public string Path { get; }
    public long Length { get; }
    public int SectorCount { get; }

    public RawDiscImage(string path)
    {
        Path = path;
        _stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read,
            1 << 20, FileOptions.SequentialScan);
        Length = _stream.Length;
        if (Length % RawSectorSize != 0)
            throw new InvalidDataException(
                $"Image size {Length} is not a multiple of {RawSectorSize}; not a raw 2352-byte-sector image.");
        SectorCount = checked((int)(Length / RawSectorSize));

        Span<byte> probe = stackalloc byte[RawSectorSize];
        ReadRawSectors(16, 1, probe);
        if (!probe[..12].SequenceEqual(SyncPattern))
            throw new InvalidDataException("Sector 16 has no CD sync pattern; not a raw 2352-byte-sector image.");
    }

    /// <summary>Reads <paramref name="count"/> raw sectors starting at <paramref name="lba"/>.</summary>
    public void ReadRawSectors(int lba, int count, Span<byte> destination)
    {
        if (lba < 0 || count < 0 || (long)lba + count > SectorCount)
            throw new ArgumentOutOfRangeException(nameof(lba),
                $"Sector range {lba}..{lba + count - 1} is outside the image (0..{SectorCount - 1}).");
        int bytes = checked(count * RawSectorSize);
        _stream.Seek((long)lba * RawSectorSize, SeekOrigin.Begin);
        _stream.ReadExactly(destination[..bytes]);
    }

    public static bool HasSync(ReadOnlySpan<byte> rawSector) => rawSector[..12].SequenceEqual(SyncPattern);

    public static SectorHeader ParseHeader(ReadOnlySpan<byte> rawSector) =>
        new(rawSector[12], rawSector[13], rawSector[14], rawSector[15],
            rawSector[16], rawSector[17], (Submode)rawSector[18], rawSector[19]);

    /// <summary>
    /// Reads the 2048-byte Form 1 user data areas of a run of sectors (used for ISO9660 structures).
    /// </summary>
    public byte[] ReadForm1(int lba, int sectorCount)
    {
        var result = new byte[sectorCount * Form1DataSize];
        var raw = new byte[RawSectorSize];
        for (int i = 0; i < sectorCount; i++)
        {
            ReadRawSectors(lba + i, 1, raw);
            raw.AsSpan(UserDataOffset, Form1DataSize).CopyTo(result.AsSpan(i * Form1DataSize));
        }
        return result;
    }

    public void Dispose() => _stream.Dispose();
}
