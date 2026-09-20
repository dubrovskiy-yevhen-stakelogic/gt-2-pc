using System.Text;

namespace Gt2Vol;

/// <summary>
/// Produces a markdown inventory of a GTFS volume: directory sizes, counts by
/// (compound) extension, payload signatures and 16-byte samples per file type.
/// Works directly on the volume; nothing needs to be extracted first.
/// </summary>
internal static class Inventory
{
    private sealed class TypeStats
    {
        public int Count;
        public long RawBytes;
        public long PayloadBytes;
        public int Gzipped;
        public long MinPayload = long.MaxValue;
        public long MaxPayload;
        public readonly List<(string Path, byte[] Head, bool Gz, long PayloadSize)> Samples = new();
        public readonly Dictionary<string, int> Magics = new();
        public readonly Dictionary<string, int> Dirs = new();
    }

    public static int Run(string volPath, string reportPath)
    {
        using var vol = new GtfsVolume(volPath);
        var files = vol.Entries.Where(e => !e.IsDirectory).ToList();

        var byType = new SortedDictionary<string, TypeStats>(StringComparer.Ordinal);
        var dirDirect = new SortedDictionary<string, (int Count, long Raw, long Payload)>(StringComparer.Ordinal);
        var gzFailures = new List<string>();

        // Pick sample positions per type after a first pass that only counts.
        var typeOf = new Dictionary<VolEntry, string>();
        var perTypeTotal = new Dictionary<string, int>();
        foreach (var f in files)
        {
            string type = TypeKey(f.FullPath);
            typeOf[f] = type;
            perTypeTotal[type] = perTypeTotal.GetValueOrDefault(type) + 1;
        }
        var perTypeSeen = new Dictionary<string, int>();

        foreach (var f in files)
        {
            string type = typeOf[f];
            if (!byType.TryGetValue(type, out var stats))
                byType[type] = stats = new TypeStats();

            byte[] raw = vol.ReadEntry(f.Record.Index);
            byte[] payload = raw;
            bool gz = Commands.IsGzip(raw);
            if (gz)
            {
                try { payload = Commands.Gunzip(raw); }
                catch (InvalidDataException) { gzFailures.Add(f.FullPath); gz = false; }
            }

            stats.Count++;
            stats.RawBytes += raw.Length;
            stats.PayloadBytes += payload.Length;
            if (gz) stats.Gzipped++;
            stats.MinPayload = Math.Min(stats.MinPayload, payload.Length);
            stats.MaxPayload = Math.Max(stats.MaxPayload, payload.Length);

            string magic = MagicKey(payload);
            stats.Magics[magic] = stats.Magics.GetValueOrDefault(magic) + 1;

            string dir = DirOf(f.FullPath);
            stats.Dirs[dir] = stats.Dirs.GetValueOrDefault(dir) + 1;
            var d = dirDirect.GetValueOrDefault(dir);
            dirDirect[dir] = (d.Count + 1, d.Raw + raw.Length, d.Payload + payload.Length);

            // Samples: first, middle and last file of each type.
            int seen = perTypeSeen.GetValueOrDefault(type);
            perTypeSeen[type] = seen + 1;
            int total = perTypeTotal[type];
            if (seen == 0 || seen == total / 2 || seen == total - 1)
            {
                if (stats.Samples.Count < 3 && stats.Samples.All(s => s.Path != f.FullPath))
                    stats.Samples.Add((f.FullPath, payload.AsSpan(0, Math.Min(16, payload.Length)).ToArray(), gz, payload.Length));
            }
        }

        var sb = new StringBuilder();
        sb.AppendLine($"# GTFS inventory: {volPath}");
        sb.AppendLine();
        sb.AppendLine($"- volume length: {vol.Length:N0} bytes");
        sb.AppendLine($"- offset entries: {vol.OffsetCount:N0}, TOC records: {vol.RecordCount:N0}");
        sb.AppendLine($"- directories: {vol.Entries.Count(e => e.IsDirectory):N0}, files: {files.Count:N0}");
        sb.AppendLine($"- stored bytes: {files.Sum(f => f.Size):N0}; payload bytes after gunzip: {byType.Values.Sum(t => t.PayloadBytes):N0}");
        sb.AppendLine($"- gzip members: {byType.Values.Sum(t => t.Gzipped):N0}; gzip failures: {gzFailures.Count}");
        sb.AppendLine();

        sb.AppendLine("## Directories (direct children only)");
        sb.AppendLine();
        sb.AppendLine("| Directory | Files | Stored bytes | Payload bytes (gunzipped) |");
        sb.AppendLine("|---|---:|---:|---:|");
        foreach (var (dir, v) in dirDirect)
            sb.AppendLine($"| `{dir}` | {v.Count:N0} | {v.Raw:N0} | {v.Payload:N0} |");
        sb.AppendLine();

        sb.AppendLine("## File types (compound extension)");
        sb.AppendLine();
        sb.AppendLine("| Type | Files | Gzipped | Stored bytes | Payload bytes | Payload min..max | Directories | Payload magics (first 4 bytes: count) |");
        sb.AppendLine("|---|---:|---:|---:|---:|---|---|---|");
        foreach (var (type, t) in byType)
        {
            string dirs = string.Join(", ", t.Dirs.OrderByDescending(p => p.Value).Take(4).Select(p => $"{p.Key} ({p.Value})"));
            if (t.Dirs.Count > 4) dirs += $", +{t.Dirs.Count - 4} more";
            string magics = string.Join("; ", t.Magics.OrderByDescending(p => p.Value).Take(4).Select(p => $"{p.Key}: {p.Value}"));
            if (t.Magics.Count > 4) magics += $"; +{t.Magics.Count - 4} more";
            sb.AppendLine($"| `{type}` | {t.Count:N0} | {t.Gzipped:N0} | {t.RawBytes:N0} | {t.PayloadBytes:N0} | {t.MinPayload:N0}..{t.MaxPayload:N0} | {dirs} | {magics} |");
        }
        sb.AppendLine();

        sb.AppendLine("## Samples (first 16 payload bytes; payload = gunzipped content when the file is a gzip member)");
        sb.AppendLine();
        sb.AppendLine("| Type | Sample | gz | Payload size | Hex | ASCII |");
        sb.AppendLine("|---|---|---|---:|---|---|");
        foreach (var (type, t) in byType)
            foreach (var s in t.Samples)
                sb.AppendLine($"| `{type}` | `{s.Path}` | {(s.Gz ? "yes" : "no")} | {s.PayloadSize:N0} | `{Hex(s.Head)}` | `{Ascii(s.Head)}` |");
        sb.AppendLine();

        if (gzFailures.Count > 0)
        {
            sb.AppendLine("## Gzip failures");
            foreach (var p in gzFailures) sb.AppendLine($"- `{p}`");
            sb.AppendLine();
        }

        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(reportPath))!);
        File.WriteAllText(reportPath, sb.ToString(), new UTF8Encoding(false));
        Console.WriteLine($"inventory written: {reportPath} ({byType.Count} types, {dirDirect.Count} directories)");
        return 0;
    }

    /// <summary>
    /// Compound extension of the file name: text after the first dot that is not the
    /// leading character (".crstims.tsd.gz" -> ".tsd.gz", ".carcolor" -> "(dotfile)").
    /// </summary>
    public static string TypeKey(string fullPath)
    {
        string name = fullPath[(fullPath.LastIndexOf('/') + 1)..];
        int dot = name.IndexOf('.', 1);
        if (dot < 0)
            return name.StartsWith('.') ? "(dotfile)" : "(none)";
        return name[dot..].ToLowerInvariant();
    }

    public static string DirOf(string fullPath)
    {
        int slash = fullPath.LastIndexOf('/');
        return slash < 0 ? "/" : "/" + fullPath[..slash];
    }

    private static string MagicKey(byte[] payload)
    {
        if (payload.Length == 0) return "(empty)";
        var head = payload.AsSpan(0, Math.Min(4, payload.Length));
        bool printable = true;
        foreach (byte b in head)
            if (b != 0 && (b < 0x20 || b > 0x7E)) { printable = false; break; }
        if (printable && head[0] != 0)
            return "'" + Ascii(head.ToArray()).TrimEnd('.') + "'";
        return Hex(head.ToArray()).Replace(" ", "");
    }

    private static string Hex(byte[] data) => string.Join(' ', data.Select(b => b.ToString("X2")));

    private static string Ascii(byte[] data)
    {
        var chars = new char[data.Length];
        for (int i = 0; i < data.Length; i++)
        {
            byte b = data[i];
            // '|' and '`' would break the markdown table cell.
            chars[i] = b >= 0x20 && b < 0x7F && b != (byte)'|' && b != (byte)'`' ? (char)b : '.';
        }
        return new string(chars);
    }
}
