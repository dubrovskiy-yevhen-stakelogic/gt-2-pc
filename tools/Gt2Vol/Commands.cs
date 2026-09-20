using System.IO.Compression;

namespace Gt2Vol;

internal static class Commands
{
    public static int Info(string volPath)
    {
        using var vol = new GtfsVolume(volPath);
        var files = vol.Entries.Where(e => !e.IsDirectory).ToList();
        var dirs = vol.Entries.Where(e => e.IsDirectory).ToList();

        Console.WriteLine($"volume          : {vol.Path}");
        Console.WriteLine($"length          : {vol.Length:N0} bytes ({vol.Length / GtfsVolume.BlockSize:N0} blocks, remainder {vol.Length % GtfsVolume.BlockSize})");
        Console.WriteLine($"offset entries  : {vol.OffsetCount:N0}");
        Console.WriteLine($"toc records     : {vol.RecordCount:N0}");
        Console.WriteLine($"entry0 (table)  : start 0x{vol.GetStart(0):X} size {vol.GetSize(0):N0} (expected {GtfsVolume.HeaderSize + vol.OffsetCount * 4:N0})");
        Console.WriteLine($"entry1 (toc)    : start 0x{vol.GetStart(1):X} size {vol.GetSize(1):N0} (expected {vol.RecordCount * GtfsVolume.RecordSize:N0})");
        int last = vol.OffsetCount - 1;
        Console.WriteLine($"sentinel entry  : raw 0x{vol.RawOffsets[last]:X8} -> 0x{vol.GetStart(last):X} (file length 0x{vol.Length:X})");
        Console.WriteLine($"directories     : {dirs.Count:N0}");
        Console.WriteLine($"files           : {files.Count:N0}");
        Console.WriteLine($"file bytes      : {files.Sum(f => f.Size):N0}");
        Console.WriteLine($"'..' records    : {vol.Records.Count(r => r.Name == ".."):N0}");

        // Consistency: offsets must be monotonic, every data entry referenced exactly once.
        int nonMonotonic = 0;
        for (int i = 1; i < vol.OffsetCount; i++)
            if (vol.GetStart(i) < vol.GetStart(i - 1)) nonMonotonic++;
        var refCount = new int[vol.OffsetCount];
        foreach (var f in files) refCount[f.Record.Index]++;
        int unreferenced = 0, multi = 0;
        for (int i = 2; i < vol.OffsetCount - 1; i++)
        {
            if (refCount[i] == 0) unreferenced++;
            else if (refCount[i] > 1) multi++;
        }
        Console.WriteLine($"non-monotonic   : {nonMonotonic}");
        Console.WriteLine($"unreferenced    : {unreferenced} data entries");
        Console.WriteLine($"multi-referenced: {multi} data entries");
        Console.WriteLine($"negative sizes  : {files.Count(f => f.Size < 0)}");
        Console.WriteLine($"zero-size files : {files.Count(f => f.Size == 0)}");
        if (files.Count > 0)
        {
            Console.WriteLine($"timestamps      : {files.Min(f => f.Record.TimeUtc):yyyy-MM-dd HH:mm:ss} .. {files.Max(f => f.Record.TimeUtc):yyyy-MM-dd HH:mm:ss} UTC");
        }
        foreach (var w in vol.Warnings)
            Console.WriteLine("WARNING: " + w);
        return 0;
    }

    public static int Toc(string volPath)
    {
        using var vol = new GtfsVolume(volPath);
        Console.WriteLine("rec\ttimestamp\tindex\tflags\tname");
        foreach (var r in vol.Records)
            Console.WriteLine($"{r.RecordIndex}\t{r.TimeUtc:yyyy-MM-dd HH:mm:ss}\t{r.Index}\t0x{r.Flags:X2}\t{r.Name}");
        return 0;
    }

    public static int List(string volPath)
    {
        using var vol = new GtfsVolume(volPath);
        Console.WriteLine("offset\tsize\ttime_utc\tpath");
        foreach (var e in vol.Entries)
        {
            if (e.IsDirectory)
                Console.WriteLine($"-\t<DIR>\t{e.Record.TimeUtc:yyyy-MM-dd HH:mm:ss}\t{e.FullPath}/");
            else
                Console.WriteLine($"0x{e.Offset:X8}\t{e.Size}\t{e.Record.TimeUtc:yyyy-MM-dd HH:mm:ss}\t{e.FullPath}");
        }
        return 0;
    }

    public static int Extract(string volPath, string outDir, bool decompress)
    {
        using var vol = new GtfsVolume(volPath);
        string root = Path.GetFullPath(outDir);
        Directory.CreateDirectory(root);

        int files = 0, gunzipped = 0, gzipFailed = 0, gzipByMagicOnly = 0, indexedContainers = 0;
        long bytes = 0, gunzippedBytes = 0;

        // A "<name>.dat" with a sibling "<name>.idx" is a container of many members
        // (see IdxSplit); gunzipping it as a single stream would yield only member 0.
        var allPaths = new HashSet<string>(vol.Entries.Select(x => x.FullPath), StringComparer.OrdinalIgnoreCase);

        foreach (var e in vol.Entries)
        {
            string target = SafeCombine(root, e.FullPath);
            if (e.IsDirectory)
            {
                Directory.CreateDirectory(target);
                continue;
            }

            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            byte[] data = vol.ReadEntry(e.Record.Index);
            File.WriteAllBytes(target, data);
            File.SetLastWriteTimeUtc(target, e.Record.TimeUtc);
            files++;
            bytes += data.Length;

            if (!decompress || !IsGzip(data))
                continue;

            if (allPaths.Contains(Path.ChangeExtension(e.FullPath, ".idx").Replace('\\', '/')))
            {
                indexedContainers++;
                continue;
            }

            bool namedGz = target.EndsWith(".gz", StringComparison.OrdinalIgnoreCase);
            string plainTarget = namedGz ? target[..^3] : target + ".ungz";
            if (!namedGz) gzipByMagicOnly++;
            try
            {
                byte[] plain = Gunzip(data);
                File.WriteAllBytes(plainTarget, plain);
                File.SetLastWriteTimeUtc(plainTarget, e.Record.TimeUtc);
                gunzipped++;
                gunzippedBytes += plain.Length;
            }
            catch (InvalidDataException ex)
            {
                gzipFailed++;
                Console.Error.WriteLine($"gunzip failed: {e.FullPath}: {ex.Message}");
            }
        }

        Console.WriteLine($"extracted {files:N0} files, {bytes:N0} bytes -> {root}");
        Console.WriteLine($"gunzipped {gunzipped:N0} files, {gunzippedBytes:N0} bytes (failed: {gzipFailed}, gzip magic without .gz name: {gzipByMagicOnly})");
        if (indexedContainers > 0)
            Console.WriteLine($"skipped gunzip of {indexedContainers} dat+idx containers (use 'split')");
        foreach (var w in vol.Warnings)
            Console.WriteLine("WARNING: " + w);
        return gzipFailed == 0 ? 0 : 3;
    }

    public static bool IsGzip(ReadOnlySpan<byte> data) =>
        data.Length >= 18 && data[0] == 0x1F && data[1] == 0x8B && data[2] == 0x08;

    public static byte[] Gunzip(byte[] data)
    {
        using var input = new MemoryStream(data, writable: false);
        using var gz = new GZipStream(input, CompressionMode.Decompress);
        using var output = new MemoryStream(data.Length * 4);
        gz.CopyTo(output);
        return output.ToArray();
    }

    /// <summary>Joins a volume path to the output root and refuses anything escaping the root.</summary>
    private static string SafeCombine(string root, string volumePath)
    {
        string relative = volumePath.Replace('/', Path.DirectorySeparatorChar);
        foreach (char c in Path.GetInvalidPathChars())
            relative = relative.Replace(c, '_');
        string full = Path.GetFullPath(Path.Combine(root, relative));
        if (!full.StartsWith(root.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException($"volume path escapes output directory: {volumePath}");
        return full;
    }
}
