using System.Security.Cryptography;
using System.Text;

namespace Gt2Vol;

/// <summary>
/// Compares two GTFS volumes by path. Files are compared by SHA-1 of the stored
/// bytes and, when both sides are gzip members, also by SHA-1 of the gunzipped
/// payload (the gzip header carries an mtime, so equal payloads can differ raw).
/// </summary>
internal static class VolDiff
{
    private sealed class Bucket
    {
        public int OnlyA, OnlyB, Same, SamePayloadOnly, Different;
        public long OnlyABytes, OnlyBBytes;
    }

    public static int Run(string volAPath, string volBPath, string reportPath)
    {
        using var volA = new GtfsVolume(volAPath);
        using var volB = new GtfsVolume(volBPath);

        var a = volA.Entries.Where(e => !e.IsDirectory).ToDictionary(e => e.FullPath, StringComparer.Ordinal);
        var b = volB.Entries.Where(e => !e.IsDirectory).ToDictionary(e => e.FullPath, StringComparer.Ordinal);

        var buckets = new SortedDictionary<string, Bucket>(StringComparer.Ordinal);
        var onlyA = new List<VolEntry>();
        var onlyB = new List<VolEntry>();
        var different = new List<(string Path, long SizeA, long SizeB)>();
        var samePayloadOnly = new List<string>();

        Bucket BucketFor(string path)
        {
            string key = Inventory.DirOf(path);
            if (!buckets.TryGetValue(key, out var bucket))
                buckets[key] = bucket = new Bucket();
            return bucket;
        }

        foreach (var (path, ea) in a.OrderBy(p => p.Key, StringComparer.Ordinal))
        {
            var bucket = BucketFor(path);
            if (!b.TryGetValue(path, out var eb))
            {
                bucket.OnlyA++;
                bucket.OnlyABytes += ea.Size;
                onlyA.Add(ea);
                continue;
            }

            byte[] da = volA.ReadEntry(ea.Record.Index);
            byte[] db = volB.ReadEntry(eb.Record.Index);
            if (da.AsSpan().SequenceEqual(db))
            {
                bucket.Same++;
                continue;
            }

            if (Commands.IsGzip(da) && Commands.IsGzip(db) && PayloadEqual(da, db))
            {
                bucket.SamePayloadOnly++;
                samePayloadOnly.Add(path);
                continue;
            }

            bucket.Different++;
            different.Add((path, ea.Size, eb.Size));
        }

        foreach (var (path, eb) in b.OrderBy(p => p.Key, StringComparer.Ordinal))
        {
            if (a.ContainsKey(path)) continue;
            var bucket = BucketFor(path);
            bucket.OnlyB++;
            bucket.OnlyBBytes += eb.Size;
            onlyB.Add(eb);
        }

        var sb = new StringBuilder();
        sb.AppendLine("# GTFS volume diff");
        sb.AppendLine();
        sb.AppendLine($"- A: `{volAPath}` ({a.Count:N0} files)");
        sb.AppendLine($"- B: `{volBPath}` ({b.Count:N0} files)");
        sb.AppendLine($"- common paths: {a.Keys.Count(b.ContainsKey):N0}; only in A: {onlyA.Count:N0}; only in B: {onlyB.Count:N0}");
        sb.AppendLine($"- identical: {buckets.Values.Sum(x => x.Same):N0}; identical after gunzip only: {samePayloadOnly.Count:N0}; different content: {different.Count:N0}");
        sb.AppendLine();

        sb.AppendLine("## Per directory");
        sb.AppendLine();
        sb.AppendLine("| Directory | Only A | Only A bytes | Only B | Only B bytes | Identical | Identical payload only | Different |");
        sb.AppendLine("|---|---:|---:|---:|---:|---:|---:|---:|");
        foreach (var (dir, k) in buckets)
            sb.AppendLine($"| `{dir}` | {k.OnlyA:N0} | {k.OnlyABytes:N0} | {k.OnlyB:N0} | {k.OnlyBBytes:N0} | {k.Same:N0} | {k.SamePayloadOnly:N0} | {k.Different:N0} |");
        sb.AppendLine();

        AppendList(sb, "Different content (same path)", different.Select(d => $"`{d.Path}` (A {d.SizeA:N0} / B {d.SizeB:N0} bytes)"));
        AppendList(sb, "Identical payload, different gzip container", samePayloadOnly.Select(p => $"`{p}`"));
        AppendList(sb, "Only in A", onlyA.Select(e => $"`{e.FullPath}` ({e.Size:N0})"));
        AppendList(sb, "Only in B", onlyB.Select(e => $"`{e.FullPath}` ({e.Size:N0})"));

        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(reportPath))!);
        File.WriteAllText(reportPath, sb.ToString(), new UTF8Encoding(false));
        Console.WriteLine($"diff written: {reportPath}");
        Console.WriteLine($"common {a.Keys.Count(b.ContainsKey):N0}, onlyA {onlyA.Count:N0}, onlyB {onlyB.Count:N0}, same {buckets.Values.Sum(x => x.Same):N0}, samePayloadOnly {samePayloadOnly.Count:N0}, different {different.Count:N0}");
        return 0;
    }

    private static bool PayloadEqual(byte[] gzA, byte[] gzB)
    {
        try
        {
            return SHA1.HashData(Commands.Gunzip(gzA)).AsSpan().SequenceEqual(SHA1.HashData(Commands.Gunzip(gzB)));
        }
        catch (InvalidDataException)
        {
            return false;
        }
    }

    private static void AppendList(StringBuilder sb, string title, IEnumerable<string> items)
    {
        var list = items.ToList();
        sb.AppendLine($"## {title} ({list.Count:N0})");
        sb.AppendLine();
        foreach (var item in list)
            sb.AppendLine("- " + item);
        sb.AppendLine();
    }
}
