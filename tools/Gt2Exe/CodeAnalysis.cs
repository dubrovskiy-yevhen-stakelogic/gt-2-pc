using System.Security.Cryptography;

namespace Gt2Exe;

/// <summary>A half-open address range [Start, End) inside a module.</summary>
public readonly record struct AddrRange(uint Start, uint End)
{
    public bool Contains(uint a) => a >= Start && a < End;
    public uint Length => End - Start;
    public override string ToString() => $"0x{Start:X8}..0x{End:X8}";
}

public sealed record FunctionInfo(uint Start, uint End, int Instructions, bool HasPrologue, bool IsJalTarget);

public sealed class CodeStats
{
    public int Words, JrRa, Prologues, StackReleases, Jal, UniqueJalTargets;
    public int JalInternal, JalExternal, Cop2, GteCommands, Lwc2Swc2, Lui1F80, Syscalls, Breaks;
    public int ImplausibleWords;
    public SortedDictionary<uint, int> GteByCommand = new();
    public SortedDictionary<uint, int> ExternalTargets = new();
}

/// <summary>Heuristic code analysis over raw MIPS words: statistics, function discovery, load address voting.</summary>
public static class CodeAnalysis
{
    public static CodeStats Stats(Module m, IReadOnlyList<AddrRange> ranges)
    {
        var s = new CodeStats();
        var targets = new HashSet<uint>();
        foreach (var r in ranges)
        {
            for (uint a = r.Start; a + 4 <= r.End; a += 4)
            {
                uint w = m.Word((int)(a - m.Base));
                s.Words++;
                if (!Mips.IsPlausible(w)) s.ImplausibleWords++;
                if (w == Mips.JrRa) s.JrRa++;
                if (Mips.IsPrologue(w)) s.Prologues++;
                if (Mips.IsStackRelease(w)) s.StackReleases++;
                if (w == 0x0000000C) s.Syscalls++;
                if ((w & 0xFC00003F) == 0x0000000D) s.Breaks++;
                if (Mips.IsLui(w) && (w & 0xFFFF) == 0x1F80) s.Lui1F80++;
                if (Mips.IsLwc2OrSwc2(w)) s.Lwc2Swc2++;
                if (Mips.IsCop2(w))
                {
                    s.Cop2++;
                    if (Mips.IsGteCommand(w))
                    {
                        s.GteCommands++;
                        uint cmd = w & 0x3F;
                        s.GteByCommand[cmd] = s.GteByCommand.GetValueOrDefault(cmd) + 1;
                    }
                }
                if (Mips.IsJal(w))
                {
                    s.Jal++;
                    uint t = Mips.JumpTarget(w, a);
                    targets.Add(t);
                    if (InRanges(ranges, t)) s.JalInternal++;
                    else
                    {
                        s.JalExternal++;
                        s.ExternalTargets[t] = s.ExternalTargets.GetValueOrDefault(t) + 1;
                    }
                }
            }
        }
        s.UniqueJalTargets = targets.Count;
        return s;
    }

    private static bool InRanges(IReadOnlyList<AddrRange> ranges, uint a)
    {
        foreach (var r in ranges) if (r.Contains(a)) return true;
        return false;
    }

    /// <summary>
    /// Discover function boundaries. A function start is: the range start, any jal target inside the
    /// range (from this module or from <paramref name="extraCallers"/>), or the first non-nop word after
    /// a "jr ra" + delay slot. Returns functions sorted by address.
    /// </summary>
    public static List<FunctionInfo> FindFunctions(Module m, IReadOnlyList<AddrRange> ranges,
        IEnumerable<uint>? extraJalTargets = null)
    {
        var jalTargets = new HashSet<uint>();
        foreach (var r in ranges)
            for (uint a = r.Start; a + 4 <= r.End; a += 4)
            {
                uint w = m.Word((int)(a - m.Base));
                if (Mips.IsJal(w)) jalTargets.Add(Mips.JumpTarget(w, a));
            }
        if (extraJalTargets != null) foreach (uint t in extraJalTargets) jalTargets.Add(t);

        var result = new List<FunctionInfo>();
        foreach (var r in ranges)
        {
            var starts = new SortedSet<uint>();
            // Range start (skip leading zero padding).
            uint first = r.Start;
            while (first + 4 <= r.End && m.Word((int)(first - m.Base)) == 0) first += 4;
            if (first + 4 <= r.End) starts.Add(first);

            foreach (uint t in jalTargets)
                if (r.Contains(t) && (t & 3) == 0) starts.Add(t);

            for (uint a = r.Start; a + 8 <= r.End; a += 4)
            {
                if (m.Word((int)(a - m.Base)) != Mips.JrRa) continue;
                uint next = a + 8;
                while (next + 4 <= r.End && m.Word((int)(next - m.Base)) == 0) next += 4;
                if (next + 4 <= r.End) starts.Add(next);
            }

            var list = starts.ToList();
            for (int i = 0; i < list.Count; i++)
            {
                uint start = list[i];
                uint end = i + 1 < list.Count ? list[i + 1] : r.End;
                // Trim trailing zero padding.
                while (end > start + 4 && m.Word((int)(end - 4 - m.Base)) == 0) end -= 4;
                // Keep the delay-slot nop after a final jr ra.
                if (end >= start + 4 && m.Word((int)(end - 4 - m.Base)) == Mips.JrRa) end += 4;
                bool prologue = false;
                uint scanEnd = Math.Min(end, start + 8 * 4);
                for (uint a = start; a < scanEnd; a += 4)
                    if (Mips.IsPrologue(m.Word((int)(a - m.Base)))) { prologue = true; break; }
                result.Add(new FunctionInfo(start, end, (int)((end - start) / 4), prologue, jalTargets.Contains(start)));
            }
        }
        return result;
    }

    /// <summary>
    /// Vote for the load address of a position-dependent blob: every (jal target, function start offset)
    /// pair votes for base = target - offset. The true base collects one vote per internal call.
    /// </summary>
    public static List<(uint Base, int Votes)> VoteLoadAddress(byte[] blob, int top = 5)
    {
        var startOffsets = new List<uint>();
        var targets = new List<uint>();
        for (int o = 0; o + 4 <= blob.Length; o += 4)
        {
            uint w = BitConverter.ToUInt32(blob, o);
            if (Mips.IsPrologue(w)) startOffsets.Add((uint)o);
            if (Mips.IsJal(w)) targets.Add((w & 0x03FFFFFF) << 2 | 0x80000000);
        }
        var votes = new Dictionary<uint, int>();
        foreach (uint t in targets.Distinct())
            foreach (uint so in startOffsets)
            {
                if (so > t - 0x80000000) continue;
                uint b = t - so;
                if ((b & 0xFFF) != 0) continue; // only page-aligned candidates keep the table small
                votes[b] = votes.GetValueOrDefault(b) + 1;
            }
        return votes.OrderByDescending(kv => kv.Value).Take(top).Select(kv => (kv.Key, kv.Value)).ToList();
    }

    /// <summary>
    /// Automatic code range detection. A 0x100-byte window is "code-like" when at most one word is an
    /// implausible opcode and it is not mostly zero. Adjacent code-like windows are merged; a run is kept
    /// only if it contains a "jr ra", and its end is trimmed to the last "jr ra" + delay slot.
    /// Embedded data that happens to decode cleanly can still slip in, so treat the result as an estimate.
    /// </summary>
    public static List<AddrRange> FindCodeRanges(Module m)
    {
        const int Window = 0x100;
        var ranges = new List<AddrRange>();
        int runStart = -1;
        int len = m.Data.Length & ~3;
        // One extra iteration past the end (off >= len is never code-like) closes a run that reaches EOF.
        for (int off = 0; off < len + Window; off += Window)
        {
            bool codeLike = false;
            if (off < len)
            {
                int n = Math.Min(Window, len - off), bad = 0, nonZero = 0;
                for (int i = 0; i + 4 <= n; i += 4)
                {
                    uint w = m.Word(off + i);
                    if (w != 0) nonZero++;
                    if (!Mips.IsPlausible(w)) bad++;
                }
                codeLike = bad <= 1 && nonZero >= n / 4 / 4;
            }
            if (codeLike) { if (runStart < 0) runStart = off; continue; }
            if (runStart < 0) continue;

            int runEnd = Math.Min(off, len);
            int lastJr = -1;
            for (int i = runStart; i + 4 <= runEnd; i += 4)
                if (m.Word(i) == Mips.JrRa) lastJr = i;
            // The function containing the last jr ra may spill into the next (mixed) window: extend the
            // search forward across that window as long as words stay plausible.
            int probeEnd = Math.Min(len, runEnd + Window);
            for (int i = runEnd; i + 4 <= probeEnd && Mips.IsPlausible(m.Word(i)); i += 4)
                if (m.Word(i) == Mips.JrRa) lastJr = i;
            if (lastJr >= 0)
                ranges.Add(new AddrRange(m.AddrOf(runStart), m.AddrOf(Math.Min(len, lastJr + 8))));
            runStart = -1;
        }
        return ranges;
    }

    /// <summary>
    /// Resolve absolute address references built from "lui rX,hi" + I-type op using rX as base
    /// (addiu/ori/load/store). Linear scan with per-register lui tracking; state is reset after "jr ra".
    /// Returns (code address of the second instruction, resolved address).
    /// </summary>
    public static IEnumerable<(uint Site, uint Target)> ResolveAbsoluteRefs(Module m, IReadOnlyList<AddrRange> ranges)
    {
        var hi = new uint?[32];
        foreach (var r in ranges)
        {
            Array.Clear(hi);
            for (uint a = r.Start; a + 4 <= r.End; a += 4)
            {
                uint w = m.Word((int)(a - m.Base));
                uint op = Mips.Op(w);
                if (op == 0x0F) { hi[Mips.Rt(w)] = (w & 0xFFFF) << 16; continue; }
                bool immBase = op is 8 or 9 || (op >= 0x20 && op <= 0x26) || (op >= 0x28 && op <= 0x2B);
                if (immBase && hi[Mips.Rs(w)] is uint h)
                    yield return (a, h + (uint)(int)Mips.Imm(w));
                else if (op == 0x0D && hi[Mips.Rs(w)] is uint h2)
                    yield return (a, h2 | (w & 0xFFFF));

                // Destination register is overwritten: forget its lui value (addiu rX,rX,lo keeps nothing useful).
                if (op is 8 or 9 or 0x0A or 0x0B or 0x0C or 0x0D or 0x0E || (op >= 0x20 && op <= 0x26))
                    hi[Mips.Rt(w)] = null;
                else if (op == 0)
                    hi[(w >> 11) & 31] = null;
                if (w == Mips.JrRa) { /* keep state through the delay slot, reset after it */ }
                else if (a >= r.Start + 4 && m.Word((int)(a - 4 - m.Base)) == Mips.JrRa) Array.Clear(hi);
            }
        }
    }

    /// <summary>Classify what a "lui rX,0x1F80" is used for by looking at the next uses of rX as a base register.</summary>
    public static string ClassifyLui1F80(Module m, int offset)
    {
        uint reg = Mips.Rt(m.Word(offset));
        for (int i = 1; i <= 24 && offset + i * 4 + 4 <= m.Data.Length; i++)
        {
            uint w = m.Word(offset + i * 4);
            uint op = Mips.Op(w);
            bool usesBase = (op is 8 or 9 or 0x0D || (op >= 0x20 && op <= 0x26) || (op >= 0x28 && op <= 0x2B) || op is 0x32 or 0x3A)
                            && Mips.Rs(w) == reg;
            if (!usesBase) continue;
            uint imm = w & 0xFFFF;
            if (imm < 0x0400) return "scratchpad";
            if (imm is >= 0x1000 and < 0x3000) return $"io:0x1F80{imm:X4}";
            return $"other:0x{imm:X4}";
        }
        return "unknown";
    }

    public static string ExactHash(Module m, FunctionInfo f)
        => Convert.ToHexString(SHA1.HashData(m.Data.AsSpan((int)(f.Start - m.Base), (int)(f.End - f.Start))), 0, 8);

    public static string MaskedHash(Module m, FunctionInfo f)
    {
        var buf = new byte[f.End - f.Start];
        for (uint a = f.Start; a < f.End; a += 4)
        {
            uint w = Mips.MaskRelocatable(m.Word((int)(a - m.Base)));
            BitConverter.TryWriteBytes(buf.AsSpan((int)(a - f.Start)), w);
        }
        return Convert.ToHexString(SHA1.HashData(buf), 0, 8);
    }

    public static double Entropy(ReadOnlySpan<byte> data)
    {
        if (data.Length == 0) return 0;
        Span<int> hist = stackalloc int[256];
        foreach (byte b in data) hist[b]++;
        double e = 0;
        foreach (int c in hist)
        {
            if (c == 0) continue;
            double p = (double)c / data.Length;
            e -= p * Math.Log2(p);
        }
        return e;
    }
}
