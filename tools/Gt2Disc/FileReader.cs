using System.Security.Cryptography;

namespace Gt2Disc;

/// <summary>Per-file statistics gathered from the raw sector headers/subheaders.</summary>
public sealed class SectorScan
{
    public int Total;
    public int Form1;
    public int Form2;
    public int NonMode2;        // sectors whose mode byte is not 2
    public int NoSync;          // sectors without a CD sync pattern (should never happen on a data track)
    public int Video;           // submode Video bit
    public int Audio;           // submode Audio bit
    public int Data;            // submode Data bit
    public int RealTime;        // submode RealTime bit
    public readonly SortedDictionary<byte, int> SubmodeHistogram = new();
    /// <summary>Coding info byte of sectors with the Audio submode bit (XA ADPCM format).</summary>
    public readonly SortedDictionary<byte, int> AudioCodingHistogram = new();
    public readonly SortedSet<byte> FileNumbers = new();
    public readonly SortedSet<byte> Channels = new();

    public bool NeedsRaw => Form2 > 0 || NonMode2 > 0 || NoSync > 0;

    public string Summary()
    {
        string hist = string.Join(",", SubmodeHistogram.Select(kv => $"{kv.Key:X2}x{kv.Value}"));
        string text = $"F1={Form1} F2={Form2} submodes[{hist}] fileno[{string.Join(",", FileNumbers)}] chan[{string.Join(",", Channels)}]";
        if (AudioCodingHistogram.Count > 0)
            text += $" xa-audio[{string.Join(",", AudioCodingHistogram.Select(kv => $"{DescribeCoding(kv.Key)}x{kv.Value}"))}]";
        return text;
    }

    /// <summary>XA coding info: bit0 stereo, bit2 18.9 kHz (else 37.8 kHz), bit4 8-bit (else 4-bit), bit6 emphasis.</summary>
    public static string DescribeCoding(byte coding)
    {
        string channels = (coding & 0x01) != 0 ? "stereo" : "mono";
        string rate = (coding & 0x04) != 0 ? "18900Hz" : "37800Hz";
        string bits = (coding & 0x10) != 0 ? "8bit" : "4bit";
        return $"{coding:X2}:{channels}/{rate}/{bits}";
    }
}

/// <summary>
/// Reads file contents from the image, either as cooked Form 1 data (2048 bytes/sector, trimmed to
/// the directory size) or as raw 2352-byte sectors (lossless, for Form 2 / interleaved XA / STR files).
/// </summary>
public static class FileReader
{
    private const int ChunkSectors = 512;

    public static SectorScan Scan(RawDiscImage image, int lba, int sectorCount)
    {
        var scan = new SectorScan { Total = sectorCount };
        ForEachRawChunk(image, lba, sectorCount, (chunk, count) =>
        {
            for (int i = 0; i < count; i++)
            {
                var sector = chunk.Slice(i * RawDiscImage.RawSectorSize, RawDiscImage.RawSectorSize);
                if (!RawDiscImage.HasSync(sector)) { scan.NoSync++; continue; }
                var h = RawDiscImage.ParseHeader(sector);
                if (h.Mode != 2) { scan.NonMode2++; continue; }
                if (h.IsForm2) scan.Form2++; else scan.Form1++;
                if ((h.Submode & Submode.Video) != 0) scan.Video++;
                if ((h.Submode & Submode.Audio) != 0)
                {
                    scan.Audio++;
                    scan.AudioCodingHistogram[h.CodingInfo] = scan.AudioCodingHistogram.GetValueOrDefault(h.CodingInfo) + 1;
                }
                if ((h.Submode & Submode.Data) != 0) scan.Data++;
                if ((h.Submode & Submode.RealTime) != 0) scan.RealTime++;
                byte sm = (byte)h.Submode;
                scan.SubmodeHistogram[sm] = scan.SubmodeHistogram.GetValueOrDefault(sm) + 1;
                scan.FileNumbers.Add(h.FileNumber);
                scan.Channels.Add(h.Channel);
            }
        });
        return scan;
    }

    /// <summary>Copies the Form 1 user data of a file (exactly <paramref name="size"/> bytes) to <paramref name="output"/>.</summary>
    public static void CopyCooked(RawDiscImage image, int lba, long size, Stream output)
    {
        int sectorCount = (int)((size + RawDiscImage.Form1DataSize - 1) / RawDiscImage.Form1DataSize);
        long remaining = size;
        ForEachRawChunk(image, lba, sectorCount, (chunk, count) =>
        {
            for (int i = 0; i < count && remaining > 0; i++)
            {
                int take = (int)Math.Min(remaining, RawDiscImage.Form1DataSize);
                output.Write(chunk.Slice(i * RawDiscImage.RawSectorSize + RawDiscImage.UserDataOffset, take));
                remaining -= take;
            }
        });
    }

    /// <summary>Copies the complete raw 2352-byte sectors of a file to <paramref name="output"/>.</summary>
    public static void CopyRaw(RawDiscImage image, int lba, int sectorCount, Stream output)
    {
        ForEachRawChunk(image, lba, sectorCount,
            (chunk, count) => output.Write(chunk[..(count * RawDiscImage.RawSectorSize)]));
    }

    /// <summary>SHA-1 of exactly the bytes that "extract" would write for this file.</summary>
    public static string Sha1(RawDiscImage image, IsoEntry entry, bool raw)
    {
        using var sha = SHA1.Create();
        using (var cs = new CryptoStream(Stream.Null, sha, CryptoStreamMode.Write))
        {
            if (raw) CopyRaw(image, entry.Lba, entry.SectorCount, cs);
            else CopyCooked(image, entry.Lba, entry.Size, cs);
        }
        return Convert.ToHexString(sha.Hash!).ToLowerInvariant();
    }

    /// <summary>
    /// Location-independent SHA-1. Raw files: bytes 16..2351 of every sector (subheader + data +
    /// EDC/ECC; in Mode 2 none of these depend on the sector address), so the same XA/STR stream
    /// placed at a different LBA on another disc still hashes equal. Form 1 files: same as Sha1.
    /// </summary>
    public static string Sha1Content(RawDiscImage image, IsoEntry entry, bool raw)
    {
        if (!raw) return Sha1(image, entry, false);
        using var sha = SHA1.Create();
        ForEachRawChunk(image, entry.Lba, entry.SectorCount, (chunk, count) =>
        {
            for (int i = 0; i < count; i++)
            {
                var s = chunk.Slice(i * RawDiscImage.RawSectorSize + RawDiscImage.SubheaderOffset,
                    RawDiscImage.RawSectorSize - RawDiscImage.SubheaderOffset);
                sha.TransformBlock(s.ToArray(), 0, s.Length, null, 0);
            }
        });
        sha.TransformFinalBlock(Array.Empty<byte>(), 0, 0);
        return Convert.ToHexString(sha.Hash!).ToLowerInvariant();
    }

    private delegate void ChunkAction(ReadOnlySpan<byte> chunk, int sectorCount);

    private static void ForEachRawChunk(RawDiscImage image, int lba, int sectorCount, ChunkAction action)
    {
        var buffer = new byte[ChunkSectors * RawDiscImage.RawSectorSize];
        int done = 0;
        while (done < sectorCount)
        {
            int n = Math.Min(ChunkSectors, sectorCount - done);
            image.ReadRawSectors(lba + done, n, buffer);
            action(buffer, n);
            done += n;
        }
    }
}
