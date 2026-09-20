using System.Globalization;
using System.Text;
using Gt2Exe;

// Gt2Exe - static analysis helpers for the Gran Turismo 2 (PS1) executable and GT2.OVL overlays.
// Read-only on inputs; writes only where --out/--extract point to.

if (args.Length < 2) { Usage(); return 1; }
string cmd = args[0].ToLowerInvariant();
var opts = new Options(args.Skip(1).ToArray());
try
{
    switch (cmd)
    {
        case "header": return Header(opts);
        case "map": return Map(opts);
        case "strings": return Strings(opts);
        case "ovl": return Ovl(opts);
        case "gzscan": return GzScan(opts);
        case "stats": return Stats(opts);
        case "funcs": return Funcs(opts);
        case "base": return BaseVote(opts);
        case "compare": return Compare(opts);
        case "bios": return Bios(opts);
        case "xref": return Xref(opts);
        case "ranges": return RangesCmd(opts);
        case "hw": return Hw(opts);
        default: Usage(); return 1;
    }
}
catch (Exception ex)
{
    Console.Error.WriteLine($"error: {ex.Message}");
    return 2;
}

static void Usage()
{
    Console.WriteLine("""
        Gt2Exe <command> <file> [options]
          header  <exe>                          dump PS-X EXE header
          map     <file> [--chunk 0x1000]        per-chunk entropy / zero% / jr-ra / ascii% map
          strings <file> [--min 5] [--out f.tsv] printable strings with category (psyq/compiler/source/path/format/debug/ident/text/noise)
          ovl     <GT2.OVL> [--extract dir]      parse overlay container, gunzip members
          gzscan  <file> [--extract dir]         find embedded gzip members anywhere in a file
          stats   <file> [--range a:b]...        instruction statistics (jr ra, prologues, jal, GTE...)
          funcs   <file> [--range a:b]... [--out f.tsv]   heuristic function list
          base    <blob>                         vote for the load address of a raw code blob
          compare <fileA> <fileB> [--range-a a:b]... [--range-b a:b]...   function-level match (exact + relocation-masked)
          bios    <file>                         list BIOS A0/B0/C0 call stubs
          xref    <file> [--range a:b]... --to a,b | --to-range a:b   code references (lui + imm pairs) to absolute addresses
          ranges  <file>                         auto-detect code ranges (estimate)
          hw      <file> [--range a:b]...        per-page hardware usage: scratchpad / I/O port refs (lui 0x1F80), GTE ops, I/O pointers in data
        Common: --base 0xADDR (raw blobs; default 0x80010000; PS-X EXE uses t_addr). All numbers are hex.
                --range accepts start:end (repeatable) or the word 'auto' (auto-detected code ranges); default = whole module.
        """);
}

static int Header(Options o)
{
    var m = Module.Load(o.File);
    if (m.Header == null) { Console.WriteLine("not a PS-X EXE"); return 1; }
    Console.WriteLine(m.Path);
    m.Header.Print(Console.Out, new FileInfo(m.Path).Length);
    return 0;
}

static int Map(Options o)
{
    var m = Module.Load(o.File, o.Base);
    int chunk = (int)o.UInt("--chunk", 0x1000);
    Console.WriteLine($"# {m.Name} base=0x{m.Base:X8} size=0x{m.Data.Length:X} chunk=0x{chunk:X}");
    Console.WriteLine("addr      entropy zero% ascii% jr_ra prolog jal  implaus% class");
    for (int off = 0; off < m.Data.Length; off += chunk)
    {
        int n = Math.Min(chunk, m.Data.Length - off);
        var span = m.Data.AsSpan(off, n);
        double ent = CodeAnalysis.Entropy(span);
        int zero = 0, ascii = 0;
        foreach (byte b in span) { if (b == 0) zero++; else if (b is >= 0x20 and <= 0x7E) ascii++; }
        int jr = 0, pro = 0, jal = 0, bad = 0, words = n / 4;
        for (int i = 0; i + 4 <= n; i += 4)
        {
            uint w = m.Word(off + i);
            if (w == Mips.JrRa) jr++;
            if (Mips.IsPrologue(w)) pro++;
            if (Mips.IsJal(w)) jal++;
            if (!Mips.IsPlausible(w)) bad++;
        }
        double badPct = words == 0 ? 0 : 100.0 * bad / words;
        string cls = zero == n ? "zero"
            : ent > 7.5 ? "packed"
            : badPct < 3 && (jr > 0 || jal > 2) ? "code"
            : badPct < 10 && (jr > 0 || jal > 2) ? "code+data"
            : "data";
        Console.WriteLine(string.Create(CultureInfo.InvariantCulture,
            $"{m.AddrOf(off):X8}  {ent,6:F2} {100.0 * zero / n,5:F0} {100.0 * ascii / n,6:F0} {jr,5} {pro,6} {jal,4} {badPct,8:F1} {cls}"));
    }
    return 0;
}

static int Strings(Options o)
{
    var m = Module.Load(o.File, o.Base);
    int min = (int)o.UInt("--min", 5);
    var found = StringScanner.Scan(m, min);
    string? outPath = o.Str("--out");
    if (outPath != null)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(outPath))!);
        using var w = new StreamWriter(outPath, false, new UTF8Encoding(false));
        w.WriteLine("file_offset\taddress\tlength\tnul\tcategory\ttext");
        foreach (var s in found)
            w.WriteLine($"0x{s.Offset + m.FileSkip:X6}\t0x{s.Address:X8}\t{s.Text.Length}\t{(s.NulTerminated ? 1 : 0)}\t{s.Category}\t{Escape(s.Text)}");
    }
    Console.WriteLine($"# {m.Name}: {found.Count} strings (min {min})");
    foreach (var g in found.GroupBy(s => s.Category).OrderByDescending(g => g.Count()))
        Console.WriteLine($"  {g.Key,-9} {g.Count()}");
    string? show = o.Str("--show");
    if (show != null)
    {
        var cats = show.Split(',');
        foreach (var s in found.Where(s => cats.Contains(s.Category) || cats.Contains("all")))
            Console.WriteLine($"0x{s.Address:X8} [{s.Category}] {Escape(s.Text)}");
    }
    return 0;
}

static string Escape(string s) => s.Replace("\\", "\\\\").Replace("\t", "\\t").Replace("\n", "\\n").Replace("\r", "\\r");

static int Ovl(Options o)
{
    byte[] file = File.ReadAllBytes(o.File);
    var entries = OvlContainer.Parse(file);
    string? dir = o.Str("--extract");
    if (dir != null) Directory.CreateDirectory(dir);
    Console.WriteLine($"# {o.File}: {file.Length} bytes, {entries.Count} members, table size 0x{BitConverter.ToUInt32(file, 0):X}");
    Console.WriteLine("idx offset    packed   unpacked  ratio  ent_packed ent_unpacked gzip(method,flags,xfl,os) mtime_utc            name  isize_ok first_word");
    long totalPacked = 0, totalUnpacked = 0;
    foreach (var e in entries)
    {
        var packedSpan = file.AsSpan((int)e.Offset, (int)e.PackedSize);
        double entP = CodeAnalysis.Entropy(packedSpan);
        totalPacked += e.PackedSize;
        if (e.Gzip == null || e.Unpacked == null)
        {
            Console.WriteLine($"{e.Index,3} 0x{e.Offset:X6} {e.PackedSize,8}  (not gzip) ent={entP:F2}");
            continue;
        }
        totalUnpacked += e.Unpacked.Length;
        double entU = CodeAnalysis.Entropy(e.Unpacked);
        bool isizeOk = e.Gzip.TrailerISize == (uint)e.Unpacked.Length;
        uint firstWord = BitConverter.ToUInt32(e.Unpacked, 0);
        Console.WriteLine(string.Create(CultureInfo.InvariantCulture,
            $"{e.Index,3} 0x{e.Offset:X6} {e.PackedSize,8} {e.Unpacked.Length,9} {(double)e.PackedSize / e.Unpacked.Length,6:F3} {entP,10:F2} {entU,12:F2} ({e.Gzip.Method},0x{e.Gzip.Flags:X2},{e.Gzip.Xfl},{e.Gzip.Os}) {e.Gzip.MTimeUtc:yyyy-MM-dd HH:mm:ss}  {e.Gzip.FileName ?? "-"}  {isizeOk}  {firstWord:X8}"));
        if (dir != null) File.WriteAllBytes(Path.Combine(dir, $"ovl{e.Index}.bin"), e.Unpacked);
    }
    uint lastEnd = entries.Max(e => e.Offset + e.PackedSize);
    Console.WriteLine($"total packed {totalPacked}, unpacked {totalUnpacked}; last member ends at 0x{lastEnd:X} (file 0x{file.Length:X}, slack {file.Length - lastEnd})");
    return 0;
}

static int GzScan(Options o)
{
    byte[] file = File.ReadAllBytes(o.File);
    string? dir = o.Str("--extract");
    if (dir != null) Directory.CreateDirectory(dir);
    int n = 0;
    for (int i = 0; i + 18 < file.Length; i++)
    {
        if (file[i] != 0x1F || file[i + 1] != 0x8B || file[i + 2] != 8 || (file[i + 3] & 0xE0) != 0) continue;
        var info = GzipInfo.TryParse(file.AsSpan(i));
        if (info == null) continue;
        byte[] unpacked;
        try { unpacked = GzipInfo.Decompress(file.AsSpan(i)); }
        catch { continue; }
        if (unpacked.Length == 0) continue;
        Console.WriteLine($"gzip @ file+0x{i:X6}: flags=0x{info.Flags:X2} xfl={info.Xfl} os={info.Os} mtime={info.MTimeUtc:yyyy-MM-dd HH:mm:ss} name={info.FileName ?? "-"} unpacked={unpacked.Length} first16={Convert.ToHexString(unpacked, 0, Math.Min(16, unpacked.Length))}");
        if (dir != null)
        {
            string name = $"gz_{i:X6}_{SafeName(info.FileName ?? "noname")}";
            File.WriteAllBytes(Path.Combine(dir, name), unpacked);
        }
        n++;
    }
    Console.WriteLine($"{n} gzip member(s) found");
    return 0;
}

static string SafeName(string s) => new(s.Select(c => char.IsLetterOrDigit(c) || c is '.' or '_' or '-' ? c : '_').ToArray());

static int Stats(Options o)
{
    var m = Module.Load(o.File, o.Base);
    var ranges = o.Ranges("--range", m);
    var s = CodeAnalysis.Stats(m, ranges);
    var funcs = CodeAnalysis.FindFunctions(m, ranges);
    Console.WriteLine($"# {m.Name} base=0x{m.Base:X8} ranges: {string.Join(", ", ranges)}");
    Console.WriteLine($"  code bytes (ranges)     : {s.Words * 4} ({s.Words} words)");
    Console.WriteLine($"  implausible words       : {s.ImplausibleWords} ({100.0 * s.ImplausibleWords / Math.Max(1, s.Words):F2}%)");
    Console.WriteLine($"  jr ra                   : {s.JrRa}");
    Console.WriteLine($"  addiu sp,sp,-X prologues: {s.Prologues}");
    Console.WriteLine($"  addiu sp,sp,+X releases : {s.StackReleases}");
    Console.WriteLine($"  jal                     : {s.Jal} (unique targets {s.UniqueJalTargets}; internal {s.JalInternal}, external {s.JalExternal})");
    Console.WriteLine($"  heuristic functions     : {funcs.Count} (with prologue {funcs.Count(f => f.HasPrologue)}, jal targets {funcs.Count(f => f.IsJalTarget)})");
    if (funcs.Count > 0)
    {
        var sizes = funcs.Select(f => f.Instructions).OrderBy(x => x).ToList();
        Console.WriteLine($"  function size (instr)   : median {sizes[sizes.Count / 2]}, mean {sizes.Average():F0}, max {sizes[^1]}, >=500 instr: {sizes.Count(x => x >= 500)}");
    }
    Console.WriteLine($"  cop2 total / GTE cmds   : {s.Cop2} / {s.GteCommands}; lwc2+swc2 {s.Lwc2Swc2}");
    if (s.GteByCommand.Count > 0)
        Console.WriteLine("  GTE commands            : " + string.Join(", ", s.GteByCommand.Select(kv => $"{(Mips.GteCommandNames.TryGetValue(kv.Key, out var nm) ? nm : $"0x{kv.Key:X2}")}={kv.Value}")));
    Console.WriteLine($"  lui rX,0x1F80 (HW I/O)  : {s.Lui1F80}");
    Console.WriteLine($"  syscall / break         : {s.Syscalls} / {s.Breaks}");
    if (o.Flag("--externals"))
    {
        Console.WriteLine("  external jal targets (addr calls):");
        foreach (var kv in s.ExternalTargets) Console.WriteLine($"    0x{kv.Key:X8} {kv.Value}");
    }
    else if (s.ExternalTargets.Count > 0)
    {
        Console.WriteLine($"  external jal targets    : {s.ExternalTargets.Count} unique, range 0x{s.ExternalTargets.Keys.First():X8}..0x{s.ExternalTargets.Keys.Last():X8}");
    }
    return 0;
}

static int Funcs(Options o)
{
    var m = Module.Load(o.File, o.Base);
    var ranges = o.Ranges("--range", m);
    var funcs = CodeAnalysis.FindFunctions(m, ranges);
    string? outPath = o.Str("--out");
    TextWriter w = outPath != null ? new StreamWriter(outPath, false, new UTF8Encoding(false)) : Console.Out;
    w.WriteLine("start\tend\tinstr\tprologue\tjal_target\texact_hash\tmasked_hash");
    foreach (var f in funcs)
        w.WriteLine($"0x{f.Start:X8}\t0x{f.End:X8}\t{f.Instructions}\t{(f.HasPrologue ? 1 : 0)}\t{(f.IsJalTarget ? 1 : 0)}\t{CodeAnalysis.ExactHash(m, f)}\t{CodeAnalysis.MaskedHash(m, f)}");
    if (outPath != null) { w.Dispose(); Console.WriteLine($"{funcs.Count} functions -> {outPath}"); }
    return 0;
}

static int BaseVote(Options o)
{
    byte[] blob = File.ReadAllBytes(o.File);
    if (Encoding.ASCII.GetString(blob, 0, 8) == "PS-X EXE") blob = blob.AsSpan(Module.ExeHeaderSize).ToArray();
    Console.WriteLine($"# {o.File}: load address votes (page-aligned candidates)");
    foreach (var (b, v) in CodeAnalysis.VoteLoadAddress(blob))
        Console.WriteLine($"  0x{b:X8}  {v} votes");
    return 0;
}

static int Compare(Options o)
{
    if (o.Positional.Count < 2) throw new ArgumentException("compare needs two files");
    var a = Module.Load(o.Positional[0], o.UIntOpt("--base-a"));
    var b = Module.Load(o.Positional[1], o.UIntOpt("--base-b"));
    var fa = CodeAnalysis.FindFunctions(a, o.Ranges("--range-a", a));
    var fb = CodeAnalysis.FindFunctions(b, o.Ranges("--range-b", b));
    int minInstr = (int)o.UInt("--min-instr", 8);

    Console.WriteLine($"# A={a.Name} ({fa.Count} funcs)  B={b.Name} ({fb.Count} funcs)");
    Console.WriteLine($"  byte-identical files: {a.Data.AsSpan().SequenceEqual(b.Data)}");

    var exactB = Multiset(fb.Select(f => CodeAnalysis.ExactHash(b, f)));
    var maskedB = Multiset(fb.Where(f => f.Instructions >= minInstr).Select(f => CodeAnalysis.MaskedHash(b, f)));
    int exactN = 0, maskedN = 0, sameAddr = 0; long exactBytes = 0, maskedBytes = 0, totalBytes = 0;
    var bByStart = fb.ToDictionary(f => f.Start);
    foreach (var f in fa)
    {
        totalBytes += f.End - f.Start;
        string eh = CodeAnalysis.ExactHash(a, f);
        if (Take(exactB, eh))
        {
            exactN++; exactBytes += f.End - f.Start;
            if (bByStart.TryGetValue(f.Start, out var g) && CodeAnalysis.ExactHash(b, g) == eh) sameAddr++;
        }
        if (f.Instructions >= minInstr && Take(maskedB, CodeAnalysis.MaskedHash(a, f)))
        {
            maskedN++; maskedBytes += f.End - f.Start;
        }
    }
    int bigA = fa.Count(f => f.Instructions >= minInstr);
    Console.WriteLine($"  exact  match: {exactN}/{fa.Count} funcs of A ({100.0 * exactN / Math.Max(1, fa.Count):F1}%), {exactBytes}/{totalBytes} bytes ({100.0 * exactBytes / Math.Max(1, totalBytes):F1}%); of those at the same address: {sameAddr}");
    Console.WriteLine($"  masked match (>= {minInstr} instr, relocations masked): {maskedN}/{bigA} funcs of A ({100.0 * maskedN / Math.Max(1, bigA):F1}%), {maskedBytes} bytes ({100.0 * maskedBytes / Math.Max(1, totalBytes):F1}% of A code)");
    return 0;

    static Dictionary<string, int> Multiset(IEnumerable<string> keys)
    {
        var d = new Dictionary<string, int>();
        foreach (string k in keys) d[k] = d.GetValueOrDefault(k) + 1;
        return d;
    }
    static bool Take(Dictionary<string, int> d, string k)
    {
        if (!d.TryGetValue(k, out int n) || n == 0) return false;
        d[k] = n - 1;
        return true;
    }
}

static int Bios(Options o)
{
    var m = Module.Load(o.File, o.Base);
    // Stub shape: li t2,0xA0|0xB0|0xC0 ; jr t2 ; li t1,N
    int n = 0;
    for (int off = 0; off + 12 <= m.Data.Length; off += 4)
    {
        uint w0 = m.Word(off), w1 = m.Word(off + 4), w2 = m.Word(off + 8);
        if ((w0 & 0xFFFFFF0F) != 0x240A0000 || w1 != 0x01400008 || (w2 & 0xFFFF0000) != 0x24090000) continue;
        uint table = w0 & 0xFF;
        if (table is not (0xA0 or 0xB0 or 0xC0)) continue;
        Console.WriteLine($"0x{m.AddrOf(off):X8}  {table:X2}:{w2 & 0xFFFF:X2}");
        n++;
    }
    Console.WriteLine($"{n} BIOS stub(s)");
    return 0;
}

static int Xref(Options o)
{
    var m = Module.Load(o.File, o.Base);
    var ranges = o.Ranges("--range", m);
    var targets = new HashSet<uint>();
    if (o.Str("--to") is { } list)
        foreach (string s in list.Split(',', StringSplitOptions.RemoveEmptyEntries))
            targets.Add(uint.Parse(s.Replace("0x", "", StringComparison.OrdinalIgnoreCase), NumberStyles.HexNumber));
    AddrRange? toRange = null;
    if (o.Str("--to-range") is { } tr)
    {
        string[] p = tr.Split(':');
        toRange = new AddrRange(uint.Parse(p[0], NumberStyles.HexNumber), uint.Parse(p[1], NumberStyles.HexNumber));
    }
    if (targets.Count == 0 && toRange == null) throw new ArgumentException("xref needs --to a,b,... or --to-range a:b");

    var funcs = CodeAnalysis.FindFunctions(m, ranges);
    uint FuncOf(uint site)
    {
        int lo = 0, hi = funcs.Count - 1, best = -1;
        while (lo <= hi)
        {
            int mid = (lo + hi) / 2;
            if (funcs[mid].Start <= site) { best = mid; lo = mid + 1; } else hi = mid - 1;
        }
        return best >= 0 ? funcs[best].Start : 0;
    }
    Console.WriteLine("site\tfunction\ttarget");
    int n = 0;
    foreach (var (site, target) in CodeAnalysis.ResolveAbsoluteRefs(m, ranges))
    {
        if (!targets.Contains(target) && !(toRange?.Contains(target) ?? false)) continue;
        Console.WriteLine($"0x{site:X8}\t0x{FuncOf(site):X8}\t0x{target:X8}");
        n++;
    }
    Console.WriteLine($"{n} reference(s)");
    return 0;
}

static int RangesCmd(Options o)
{
    var m = Module.Load(o.File, o.Base);
    var ranges = CodeAnalysis.FindCodeRanges(m);
    Console.WriteLine($"# {m.Name} base=0x{m.Base:X8}: {ranges.Count} code range(s), {ranges.Sum(r => (long)r.Length)} bytes");
    foreach (var r in ranges) Console.WriteLine($"  {r}  {r.Length,8} bytes");
    Console.WriteLine("  args: " + string.Join(" ", ranges.Select(r => $"--range {r.Start:X8}:{r.End:X8}")));
    return 0;
}

static int Hw(Options o)
{
    var m = Module.Load(o.File, o.Base);
    var ranges = o.Ranges("--range", m);
    // page -> (scratchpad, io, other, gte commands, cop2 reg moves)
    var pages = new SortedDictionary<uint, int[]>();
    var ioPorts = new SortedDictionary<string, int>();
    var dataRefs = new SortedDictionary<uint, List<uint>>();
    int[] Page(uint a) => pages.TryGetValue(a & 0xFFFFF000, out var p) ? p : pages[a & 0xFFFFF000] = new int[5];

    for (int off = 0; off + 4 <= m.Data.Length; off += 4)
    {
        uint a = m.AddrOf(off), w = m.Word(off);
        bool inCode = ranges.Any(r => r.Contains(a));
        if (!inCode)
        {
            if (w >= 0x1F801000 && w < 0x1F803000)
            {
                if (!dataRefs.TryGetValue(w, out var l)) dataRefs[w] = l = new List<uint>();
                l.Add(a);
            }
            continue;
        }
        if (Mips.IsLui(w) && (w & 0xFFFF) == 0x1F80)
        {
            string kind = CodeAnalysis.ClassifyLui1F80(m, off);
            if (kind == "scratchpad") Page(a)[0]++;
            else if (kind.StartsWith("io:"))
            {
                Page(a)[1]++;
                ioPorts[kind] = ioPorts.GetValueOrDefault(kind) + 1;
                if (o.Flag("--list")) Console.WriteLine($"  io site 0x{a:X8} -> {kind[3..]}");
            }
            else Page(a)[2]++;
        }
        if (Mips.IsGteCommand(w)) Page(a)[3]++;
        else if (Mips.IsCop2(w) || Mips.IsLwc2OrSwc2(w)) Page(a)[4]++;
    }
    Console.WriteLine($"# {m.Name} base=0x{m.Base:X8} ranges: {string.Join(", ", ranges)}");
    Console.WriteLine("page        lui1F80:scratch  lui1F80:io  lui1F80:other  gte_cmds  cop2_reg_moves");
    foreach (var kv in pages)
        Console.WriteLine($"0x{kv.Key:X8}  {kv.Value[0],15} {kv.Value[1],11} {kv.Value[2],14} {kv.Value[3],9} {kv.Value[4],15}");
    Console.WriteLine($"totals      {pages.Values.Sum(p => p[0]),15} {pages.Values.Sum(p => p[1]),11} {pages.Values.Sum(p => p[2]),14} {pages.Values.Sum(p => p[3]),9} {pages.Values.Sum(p => p[4]),15}");
    if (ioPorts.Count > 0)
        Console.WriteLine("I/O ports referenced from code: " + string.Join(", ", ioPorts.Select(kv => $"{kv.Key[3..]} x{kv.Value}")));
    Console.WriteLine($"data words pointing at I/O ports (0x1F801000..0x1F802FFF): {dataRefs.Sum(kv => kv.Value.Count)}");
    foreach (var kv in dataRefs)
        Console.WriteLine($"  0x{kv.Key:X8} x{kv.Value.Count} at {string.Join(",", kv.Value.Take(4).Select(a => $"0x{a:X8}"))}");
    return 0;
}

/// <summary>Tiny argument parser: positional args plus "--key value" / "--flag" options (repeatable).</summary>
internal sealed class Options
{
    public List<string> Positional { get; } = new();
    private readonly List<(string Key, string? Value)> _opts = new();
    private static readonly HashSet<string> Flags = new() { "--externals", "--list" };

    public Options(string[] args)
    {
        for (int i = 0; i < args.Length; i++)
        {
            if (!args[i].StartsWith("--")) { Positional.Add(args[i]); continue; }
            if (Flags.Contains(args[i]) || i + 1 >= args.Length) _opts.Add((args[i], null));
            else { _opts.Add((args[i], args[i + 1])); i++; }
        }
        if (Positional.Count == 0) throw new ArgumentException("missing input file");
    }

    public string File => Positional[0];
    public uint? Base => UIntOpt("--base");
    public bool Flag(string key) => _opts.Any(o => o.Key == key);
    public string? Str(string key) => _opts.FirstOrDefault(o => o.Key == key).Value;
    public uint UInt(string key, uint def) => UIntOpt(key) ?? def;
    public uint? UIntOpt(string key) => Str(key) is { } s ? ParseUInt(s) : null;

    public List<AddrRange> Ranges(string key, Module m)
    {
        var list = new List<AddrRange>();
        foreach (var (k, v) in _opts)
        {
            if (k != key || v == null) continue;
            if (v.Equals("auto", StringComparison.OrdinalIgnoreCase))
            {
                list.AddRange(CodeAnalysis.FindCodeRanges(m));
                continue;
            }
            string[] p = v.Split(':');
            if (p.Length != 2) throw new ArgumentException($"bad range '{v}', expected start:end");
            var r = new AddrRange(ParseUInt(p[0]), ParseUInt(p[1]));
            if (r.Start < m.Base || r.End > m.End || r.End <= r.Start)
                throw new ArgumentException($"range {r} outside module 0x{m.Base:X8}..0x{m.End:X8}");
            list.Add(r);
        }
        if (list.Count == 0) list.Add(new AddrRange(m.Base, m.End & ~3u));
        return list;
    }

    private static uint ParseUInt(string s) =>
        s.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
            ? uint.Parse(s.AsSpan(2), NumberStyles.HexNumber)
            : uint.Parse(s, NumberStyles.HexNumber);
}
