using System.Buffers.Binary;
using System.Text;

namespace Gt2Vol;

/// <summary>
/// Splits a GT2 "dat + idx" pair (gtmenu/&lt;lang&gt;/gtmenudat.dat, gtmenu/commonpic.dat).
///
/// idx layout (little-endian, verified on SCUS-94488 v1.2):
///   u32 count
///   u32 offsets[count + 1]   last value = end of data
/// Each offset: bits 31..2 = byte position / 4 * 4 (entries are 4-byte aligned),
/// bits 1..0 = number of padding bytes at the end of that entry.
/// Members that start with the gzip magic are written decompressed.
/// </summary>
internal static class IdxSplit
{
    public static int Run(string datPath, string idxPath, string outDir)
    {
        byte[] idx = File.ReadAllBytes(idxPath);
        byte[] dat = File.ReadAllBytes(datPath);

        int count = (int)BinaryPrimitives.ReadUInt32LittleEndian(idx);
        if (idx.Length != 4 + (count + 1) * 4)
            throw new InvalidDataException($"idx size {idx.Length} does not match count {count} (expected {4 + (count + 1) * 4})");

        var raw = new uint[count + 1];
        for (int i = 0; i <= count; i++)
            raw[i] = BinaryPrimitives.ReadUInt32LittleEndian(idx.AsSpan(4 + i * 4));

        long end = raw[count] & ~3u;
        if (end != dat.Length)
            Console.WriteLine($"WARNING: last idx offset 0x{end:X} != dat length 0x{dat.Length:X}");

        Directory.CreateDirectory(outDir);
        int gz = 0, plain = 0, failed = 0, empty = 0, nonZeroPadding = 0;
        long payloadBytes = 0;
        var magics = new SortedDictionary<string, int>(StringComparer.Ordinal);
        var sizes = new SortedDictionary<long, int>();
        var manifest = new StringBuilder("index\toffset\tstored\tpayload\tgz\tmagic\n");

        for (int i = 0; i < count; i++)
        {
            long start = raw[i] & ~3u;
            long pad = raw[i] & 3u;
            long next = raw[i + 1] & ~3u;
            long size = next - start - pad;
            if (size < 0 || next > dat.Length)
                throw new InvalidDataException($"entry {i}: bad range 0x{start:X}..0x{next:X} pad {pad}");
            if (size == 0) { empty++; manifest.Append($"{i}\t0x{start:X}\t0\t0\tno\t(empty)\n"); continue; }

            for (long p = start + size; p < next; p++)
                if (dat[p] != 0) { nonZeroPadding++; break; }

            byte[] member = dat.AsSpan((int)start, (int)size).ToArray();
            byte[] payload = member;
            bool isGz = Commands.IsGzip(member);
            if (isGz)
            {
                try { payload = Commands.Gunzip(member); gz++; }
                catch (InvalidDataException) { failed++; isGz = false; }
            }
            else plain++;

            string magic = payload.Length >= 4 ? Encoding.ASCII.GetString(payload, 0, 4) : "";
            bool printable = magic.Length == 4 && magic.All(c => c == '\0' || (c >= ' ' && c <= '~')) && magic[0] != '\0';
            string magicKey = printable ? "'" + magic.TrimEnd('\0') + "'" : Convert.ToHexString(payload.AsSpan(0, Math.Min(4, payload.Length)));
            magics[magicKey] = magics.GetValueOrDefault(magicKey) + 1;
            sizes[payload.Length] = sizes.GetValueOrDefault(payload.Length) + 1;
            payloadBytes += payload.Length;

            File.WriteAllBytes(Path.Combine(outDir, $"{i:D4}.bin"), payload);
            manifest.Append($"{i}\t0x{start:X}\t{size}\t{payload.Length}\t{(isGz ? "yes" : "no")}\t{magicKey}\n");
        }

        File.WriteAllText(Path.Combine(outDir, "_manifest.tsv"), manifest.ToString(), new UTF8Encoding(false));
        Console.WriteLine($"{Path.GetFileName(datPath)}: {count:N0} entries -> {outDir}");
        Console.WriteLine($"  gzip members {gz:N0}, plain {plain:N0}, empty {empty:N0}, gunzip failures {failed}, entries with non-zero padding {nonZeroPadding}");
        Console.WriteLine($"  payload bytes {payloadBytes:N0}; magics: {string.Join(", ", magics.Select(p => $"{p.Key}={p.Value}"))}");
        Console.WriteLine($"  distinct payload sizes: {sizes.Count}; most common: {string.Join(", ", sizes.OrderByDescending(p => p.Value).Take(5).Select(p => $"{p.Key:N0}x{p.Value}"))}");
        return failed == 0 ? 0 : 3;
    }
}
