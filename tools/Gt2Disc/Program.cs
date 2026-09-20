using System.Globalization;
using System.Text;

namespace Gt2Disc;

/// <summary>
/// Gt2Disc - reader for raw 2352-byte-sector PS1 (MODE2) disc images.
/// The image is always opened read-only.
/// </summary>
public static class Program
{
    private const string Usage = """
        Gt2Disc - PS1 raw (MODE2/2352) disc image reader

        Usage:
          Gt2Disc info    <image.bin>
          Gt2Disc list    <image.bin> [--scan]
          Gt2Disc map     <image.bin>
          Gt2Disc extract <image.bin> <outdir> [--only <path>[,<path>...]]
          Gt2Disc hash    <image.bin>
          Gt2Disc compare <imageA.bin> <imageB.bin>
          Gt2Disc exeinfo <image.bin> [<path-of-exe-on-disc>]

        list     full tree with LBA, size, XA attributes; --scan also reads every sector subheader
        map      extents sorted by LBA, including gaps not referenced by any directory record
        extract  Form1 files as plain files; Form2 / interleaved XA / STR files as raw 2352-byte
                 sectors with the suffix ".raw2352" (decided by XA flags OR by sector submode bits)
        hash     SHA-1 of every file (same bytes that extract would write), TSV to stdout
        compare  SHA-1 compare of two images by path
        exeinfo  PS-X EXE header of the boot executable named in SYSTEM.CNF (or of the given path)
        """;

    public static int Main(string[] args)
    {
        CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
        Console.OutputEncoding = new UTF8Encoding(false);
        if (args.Length < 2)
        {
            Console.Error.WriteLine(Usage);
            return 2;
        }
        try
        {
            switch (args[0].ToLowerInvariant())
            {
                case "info": return Info(args[1]);
                case "list": return List(args[1], args.Contains("--scan"));
                case "map": return Map(args[1]);
                case "extract":
                    if (args.Length < 3) break;
                    return Extract(args[1], args[2], OptionValue(args, "--only"));
                case "hash": return Hash(args[1]);
                case "compare":
                    if (args.Length < 3) break;
                    return Compare(args[1], args[2]);
                case "exeinfo": return ExeInfo(args[1], args.Length > 2 ? args[2] : null);
            }
            Console.Error.WriteLine(Usage);
            return 2;
        }
        catch (Exception ex) when (ex is IOException or InvalidDataException or ArgumentException)
        {
            Console.Error.WriteLine($"error: {ex.Message}");
            return 1;
        }
    }

    private static string? OptionValue(string[] args, string name)
    {
        int i = Array.IndexOf(args, name);
        return i >= 0 && i + 1 < args.Length ? args[i + 1] : null;
    }

    // ---------------------------------------------------------------- info

    private static int Info(string imagePath)
    {
        using var image = new RawDiscImage(imagePath);
        var vol = new IsoVolume(image);
        PrintInfo(image, vol);
        return 0;
    }

    private static void PrintInfo(RawDiscImage image, IsoVolume vol)
    {
        var p = vol.Pvd;
        Console.WriteLine($"image          : {image.Path}");
        Console.WriteLine($"image size     : {image.Length} bytes = {image.SectorCount} raw sectors");
        Console.WriteLine($"system id      : {p.SystemId}");
        Console.WriteLine($"volume id      : {p.VolumeId}");
        Console.WriteLine($"volume space   : {p.VolumeSpaceSize} sectors (image has {image.SectorCount - (long)p.VolumeSpaceSize} extra)");
        Console.WriteLine($"volume set id  : {p.VolumeSetId}");
        Console.WriteLine($"publisher      : {p.PublisherId}");
        Console.WriteLine($"data preparer  : {p.DataPreparerId}");
        Console.WriteLine($"application    : {p.ApplicationId}");
        Console.WriteLine($"copyright file : {p.CopyrightFileId}");
        Console.WriteLine($"created        : {p.CreationDate}");
        Console.WriteLine($"modified       : {p.ModificationDate}");
        Console.WriteLine($"CD-XA001 sig   : {p.HasXaSignature}");
        Console.WriteLine($"path table     : LBA {p.PathTableLba}, {p.PathTableSize} bytes");
        Console.WriteLine($"root directory : LBA {p.RootLba}, {p.RootSize} bytes");

        // License text lives in the system area (sector 4 on PS1 discs).
        byte[] lic = image.ReadForm1(4, 1);
        int end = 0;
        while (end < 120 && lic[end] >= 0x20 && lic[end] < 0x7F) end++;
        string licText = Encoding.ASCII.GetString(lic, 0, end).Trim();
        Console.WriteLine($"license sector : \"{System.Text.RegularExpressions.Regex.Replace(licText, @"\s+", " ")}\"");
    }

    // ---------------------------------------------------------------- list

    private static int List(string imagePath, bool scan)
    {
        using var image = new RawDiscImage(imagePath);
        var vol = new IsoVolume(image);
        PrintInfo(image, vol);
        Console.WriteLine();
        Console.WriteLine("XA flags: D=directory 1=Form1 2=Form2 I=interleaved A=CDDA; '-----' = no XA system-use entry");
        Console.WriteLine();
        Console.WriteLine($"{"LBA",7} {"SIZE",10} {"SECT",6} {"XAATTR",6} {"XA",5} {"FN",3} {"RECORDED",-19} {"AS",-4} PATH{(scan ? "  | sector scan" : "")}");

        int files = 0, dirs = 0;
        long totalBytes = 0;
        foreach (var e in vol.Walk())
        {
            string date = e.Recorded?.ToString("yyyy-MM-dd HH:mm:ss") ?? "";
            string xaHex = e.HasXa ? ((ushort)e.Xa).ToString("X4") : "";
            string kind, scanText = "";
            if (e.IsDirectory)
            {
                dirs++;
                kind = "dir";
            }
            else
            {
                files++;
                totalBytes += e.Size;
                SectorScan? s = scan ? FileReader.Scan(image, e.Lba, e.SectorCount) : null;
                bool raw = DecideRaw(image, e, s);
                kind = raw ? "raw" : "f1";
                if (s != null) scanText = "  | " + s.Summary();
            }
            Console.WriteLine($"{e.Lba,7} {e.Size,10} {e.SectorCount,6} {xaHex,6} {e.XaFlagString(),5} {e.XaFileNumber,3} {date,-19} {kind,-4} {e.FullPath}{(e.IsDirectory ? "/" : "")}{scanText}");
        }
        Console.WriteLine();
        Console.WriteLine($"{dirs} directories, {files} files, {totalBytes} bytes (sum of directory record sizes)");
        return 0;
    }

    /// <summary>
    /// A file is extracted raw when the XA attributes say Form2/interleaved/CDDA, or when the sector
    /// subheaders disagree with a "Form1" directory flag (full scan if available, else a sparse probe).
    /// </summary>
    private static bool DecideRaw(RawDiscImage image, IsoEntry e, SectorScan? fullScan)
    {
        if (e.XaSaysRaw) return true;
        if (e.SectorCount == 0) return false;
        if (fullScan != null) return fullScan.NeedsRaw;
        return FileReader.Scan(image, e.Lba, e.SectorCount).NeedsRaw;
    }

    // ---------------------------------------------------------------- map

    private static int Map(string imagePath)
    {
        using var image = new RawDiscImage(imagePath);
        var vol = new IsoVolume(image);

        var extents = new List<(int Lba, int Count, string Label)>
        {
            (0, 16, "<system area>"),
            (16, 1, "<PVD>"),
            (vol.Root.Lba, vol.Root.SectorCount, "<root directory>"),
        };
        // Standard ISO9660 structures that no directory record points to.
        byte[] vd = image.ReadForm1(17, 1);
        if (vd[0] == 0xFF && Encoding.ASCII.GetString(vd, 1, 5) == "CD001")
            extents.Add((17, 1, "<volume descriptor set terminator>"));
        int ptSectors = (int)((vol.Pvd.PathTableSize + 2047) / 2048);
        foreach (var (lba, label) in vol.Pvd.PathTableLocations())
            if (lba > 0) extents.Add((lba, ptSectors, label));
        foreach (var e in vol.Walk())
            if (e.SectorCount > 0)
                extents.Add((e.Lba, e.SectorCount, e.FullPath + (e.IsDirectory ? "/" : "")));
        extents.Sort((a, b) => a.Lba != b.Lba ? a.Lba.CompareTo(b.Lba) : a.Count.CompareTo(b.Count));

        Console.WriteLine($"{"LBA",7} {"END",7} {"SECT",7}  WHAT");
        int cursor = 0;
        foreach (var x in extents)
        {
            if (x.Lba > cursor) PrintGap(image, cursor, x.Lba - cursor);
            string note = x.Lba < cursor ? "   !! overlaps previous extent" : "";
            Console.WriteLine($"{x.Lba,7} {x.Lba + x.Count - 1,7} {x.Count,7}  {x.Label}{note}");
            cursor = Math.Max(cursor, x.Lba + x.Count);
        }
        if (cursor < image.SectorCount) PrintGap(image, cursor, image.SectorCount - cursor);
        Console.WriteLine();
        Console.WriteLine($"volume space size (PVD) = {vol.Pvd.VolumeSpaceSize}, image sectors = {image.SectorCount}");
        return 0;
    }

    private static void PrintGap(RawDiscImage image, int lba, int count)
    {
        // Classify the unreferenced area: how many sectors carry any non-zero user data.
        int nonZero = 0, form2 = 0, noSync = 0;
        var buf = new byte[RawDiscImage.RawSectorSize];
        for (int i = 0; i < count; i++)
        {
            image.ReadRawSectors(lba + i, 1, buf);
            if (!RawDiscImage.HasSync(buf)) { noSync++; continue; }
            var h = RawDiscImage.ParseHeader(buf);
            if (h.IsForm2) form2++;
            if (buf.AsSpan(RawDiscImage.UserDataOffset, RawDiscImage.Form1DataSize).IndexOfAnyExcept((byte)0) >= 0) nonZero++;
        }
        Console.WriteLine($"{lba,7} {lba + count - 1,7} {count,7}  <gap: unreferenced; nonzero-data sectors={nonZero}, form2={form2}, nosync={noSync}>");
    }

    // ---------------------------------------------------------------- extract

    private static int Extract(string imagePath, string outDir, string? only)
    {
        using var image = new RawDiscImage(imagePath);
        var vol = new IsoVolume(image);
        HashSet<string>? filter = only?.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Select(p => p.Replace('\\', '/').Trim('/')).ToHashSet(StringComparer.OrdinalIgnoreCase);

        string outRoot = Path.GetFullPath(outDir);
        Directory.CreateDirectory(outRoot);
        int count = 0, rawCount = 0;
        long bytes = 0;

        foreach (var e in vol.Walk())
        {
            if (filter != null && !filter.Contains(e.FullPath)) continue;
            string target = Path.GetFullPath(Path.Combine(outRoot, e.FullPath.Replace('/', Path.DirectorySeparatorChar)));
            if (!target.StartsWith(outRoot, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException($"Refusing path outside output directory: {e.FullPath}");

            if (e.IsDirectory)
            {
                Directory.CreateDirectory(target);
                continue;
            }
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);

            if ((long)e.Lba + e.SectorCount > image.SectorCount)
            {
                Console.Error.WriteLine($"warning: {e.FullPath} (LBA {e.Lba}, {e.SectorCount} sectors) lies outside this image; skipped");
                continue;
            }

            bool raw = DecideRaw(image, e, null);
            if (raw) target += ".raw2352";
            using (var fs = new FileStream(target, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20))
            {
                if (raw) FileReader.CopyRaw(image, e.Lba, e.SectorCount, fs);
                else FileReader.CopyCooked(image, e.Lba, e.Size, fs);
                bytes += fs.Length;
            }
            if (e.Recorded is DateTime dt)
            {
                try { File.SetLastWriteTime(target, dt); } catch (Exception ex) when (ex is IOException or ArgumentException) { }
            }
            count++;
            if (raw) rawCount++;
            Console.WriteLine($"{(raw ? "raw" : "f1 "),-3} {e.Size,10}  {e.FullPath}{(raw ? ".raw2352" : "")}");
        }
        Console.WriteLine();
        Console.WriteLine($"extracted {count} files ({rawCount} as raw 2352-byte sectors), {bytes} bytes written to {outRoot}");
        return 0;
    }

    // ---------------------------------------------------------------- hash / compare

    /// <summary>
    /// Sha1 = location-independent content hash (see FileReader.Sha1Content); FileSha1 = hash of the
    /// exact bytes written by "extract" (differs from Sha1 only for raw files).
    /// </summary>
    private static List<(string Path, string Sha1, string FileSha1, long Size, bool Raw)> HashAll(
        RawDiscImage image, IsoVolume vol, bool withFileHash)
    {
        var result = new List<(string, string, string, long, bool)>();
        foreach (var e in vol.Files())
        {
            if ((long)e.Lba + e.SectorCount > image.SectorCount) continue;
            bool raw = DecideRaw(image, e, null);
            long size = raw ? (long)e.SectorCount * RawDiscImage.RawSectorSize : e.Size;
            string content = FileReader.Sha1Content(image, e, raw);
            string file = !withFileHash ? "" : raw ? FileReader.Sha1(image, e, true) : content;
            result.Add((e.FullPath, content, file, size, raw));
        }
        return result;
    }

    private static int Hash(string imagePath)
    {
        using var image = new RawDiscImage(imagePath);
        var vol = new IsoVolume(image);
        Console.WriteLine("sha1_extracted_file\tsha1_content_location_independent\tbytes\tkind\tpath");
        foreach (var h in HashAll(image, vol, withFileHash: true))
            Console.WriteLine($"{h.FileSha1}\t{h.Sha1}\t{h.Size}\t{(h.Raw ? "raw2352" : "form1")}\t{h.Path}");
        return 0;
    }

    private static int Compare(string pathA, string pathB)
    {
        using var imageA = new RawDiscImage(pathA);
        using var imageB = new RawDiscImage(pathB);
        var a = HashAll(imageA, new IsoVolume(imageA), withFileHash: false).ToDictionary(x => x.Path, StringComparer.OrdinalIgnoreCase);
        var b = HashAll(imageB, new IsoVolume(imageB), withFileHash: false).ToDictionary(x => x.Path, StringComparer.OrdinalIgnoreCase);

        Console.WriteLine($"A = {pathA}");
        Console.WriteLine($"B = {pathB}");
        Console.WriteLine("SHA-1 is location independent: raw (XA/STR) files are hashed over bytes 16..2351 of each sector.");
        Console.WriteLine();
        Console.WriteLine($"{"STATUS",-10} {"SIZE_A",10} {"SIZE_B",10}  {"SHA1_A",-40}  {"SHA1_B",-40}  PATH");
        foreach (string path in a.Keys.Union(b.Keys, StringComparer.OrdinalIgnoreCase).OrderBy(p => p, StringComparer.OrdinalIgnoreCase))
        {
            bool inA = a.TryGetValue(path, out var fa);
            bool inB = b.TryGetValue(path, out var fb);
            string status = !inA ? "only-B" : !inB ? "only-A" : fa.Sha1 == fb.Sha1 ? "identical" : "different";
            Console.WriteLine($"{status,-10} {(inA ? fa.Size.ToString() : "-"),10} {(inB ? fb.Size.ToString() : "-"),10}  {(inA ? fa.Sha1 : "-"),-40}  {(inB ? fb.Sha1 : "-"),-40}  {path}");
        }

        // Same content under a different name (e.g. SCUS_944.55 vs SCUS_944.88).
        var bySha = b.Values.GroupBy(x => x.Sha1).ToDictionary(g => g.Key, g => g.Select(x => x.Path).ToList());
        var renamed = a.Values.Where(x => bySha.TryGetValue(x.Sha1, out var l) && l.Any(p => !p.Equals(x.Path, StringComparison.OrdinalIgnoreCase))).ToList();
        if (renamed.Count > 0)
        {
            Console.WriteLine();
            Console.WriteLine("Same content under a different path:");
            foreach (var x in renamed)
                Console.WriteLine($"  A:{x.Path}  ==  B:{string.Join(", B:", bySha[x.Sha1])}");
        }
        return 0;
    }

    // ---------------------------------------------------------------- exeinfo

    private static int ExeInfo(string imagePath, string? exePath)
    {
        using var image = new RawDiscImage(imagePath);
        var vol = new IsoVolume(image);

        if (exePath == null)
        {
            var cnf = vol.Find("SYSTEM.CNF") ?? throw new InvalidDataException("SYSTEM.CNF not found.");
            using var ms = new MemoryStream();
            FileReader.CopyCooked(image, cnf.Lba, cnf.Size, ms);
            string text = Encoding.ASCII.GetString(ms.ToArray());
            Console.WriteLine("SYSTEM.CNF:");
            foreach (string line in text.Split('\n'))
                if (line.Trim().Length > 0) Console.WriteLine("  | " + line.TrimEnd('\r'));
            // BOOT = cdrom:\SCUS_944.88;1
            var m = System.Text.RegularExpressions.Regex.Match(text, @"BOOT\s*=\s*cdrom:\\?([^;\r\n]+)", System.Text.RegularExpressions.RegexOptions.IgnoreCase);
            if (!m.Success) throw new InvalidDataException("No BOOT= line in SYSTEM.CNF.");
            exePath = m.Groups[1].Value.Trim();
            Console.WriteLine();
        }

        var exe = vol.Find(exePath) ?? throw new InvalidDataException($"{exePath} not found on disc.");
        using var buf = new MemoryStream();
        FileReader.CopyCooked(image, exe.Lba, Math.Min(exe.Size, PsxExeHeader.HeaderSize), buf);
        var header = PsxExeHeader.Parse(buf.ToArray());
        Console.WriteLine($"{exe.FullPath} (LBA {exe.Lba}):");
        header.Print(Console.Out, exe.Size);
        return header.IsValid ? 0 : 1;
    }
}
