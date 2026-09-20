using System.Text;
using System.Text.RegularExpressions;

namespace Gt2Exe;

public sealed record FoundString(int Offset, uint Address, string Text, bool NulTerminated, string Category);

/// <summary>Printable-ASCII string extraction with a coarse "what kind of string is this" classifier.</summary>
public static partial class StringScanner
{
    public static List<FoundString> Scan(Module m, int minLength)
    {
        var result = new List<FoundString>();
        byte[] d = m.Data;
        int i = 0;
        while (i < d.Length)
        {
            if (!IsPrintable(d[i])) { i++; continue; }
            int start = i;
            while (i < d.Length && IsPrintable(d[i])) i++;
            int len = i - start;
            if (len < minLength) continue;
            string text = Encoding.ASCII.GetString(d, start, len);
            bool nul = i < d.Length && d[i] == 0;
            result.Add(new FoundString(start, m.AddrOf(start), text, nul, Classify(text, nul)));
        }
        return result;
    }

    private static bool IsPrintable(byte b) => b is >= 0x20 and <= 0x7E or (byte)'\t' or (byte)'\n' or (byte)'\r';

    /// <summary>Categories: psyq, compiler, source, path, format, debug, ident, text, noise.</summary>
    public static string Classify(string s, bool nulTerminated)
    {
        string t = s.Trim();
        if (PsyqRegex().IsMatch(t)) return "psyq";
        if (CompilerRegex().IsMatch(t)) return "compiler";
        if (SourceRegex().IsMatch(t)) return "source";
        if (PathRegex().IsMatch(t)) return "path";
        if (FormatRegex().IsMatch(t)) return "format";
        if (DebugRegex().IsMatch(t)) return "debug";
        if (LooksLikeNoise(t, nulTerminated)) return "noise";
        if (IdentRegex().IsMatch(t)) return "ident";
        return "text";
    }

    private static bool LooksLikeNoise(string t, bool nulTerminated)
    {
        if (t.Length == 0) return true;
        int alnum = 0, punct = 0;
        foreach (char c in t)
        {
            if (char.IsLetterOrDigit(c) || c == ' ') alnum++;
            else punct++;
        }
        double ratio = (double)alnum / t.Length;
        if (ratio < 0.75) return true;
        // Short strings that are not NUL-terminated are nearly always instruction bytes.
        if (!nulTerminated && t.Length < 8) return true;
        // Runs of one repeated character.
        if (t.Distinct().Count() <= 2) return true;
        return false;
    }

    [GeneratedRegex(@"Library Programs|\$Id:|Sony Computer|\blib(gpu|gte|spu|snd|cd|ds|etc|api|card|press|pad|tap|gun|mcrd|mcx|sio|comb|math|c2?|sn|hmd|gs)\b|\bCD_|\bDS_|CdInit|CdRead|VSync|ResetGraph|DrawSync|SpuInit|SPU:|_spu_|PadInit|MDEC|DecDCT|\bbios\b|SsInit|SsSeq|VAB|Psy-?Q|\bSN ?Systems|snmain|__SN_", RegexOptions.IgnoreCase)]
    private static partial Regex PsyqRegex();

    [GeneratedRegex(@"\bGCC\b|\bgcc|\bGNU\b|cc1|__builtin|pure virtual|__pure_virtual|__rtti|type_info|\bcygnus\b|egcs|libstdc|libgcc|terminate|__throw|__eh_|bad_cast|bad_alloc", RegexOptions.None)]
    private static partial Regex CompilerRegex();

    [GeneratedRegex(@"[\w\-/\\]+\.(c|cc|cpp|cxx|h|hpp|hh|s|asm|inc|o|obj)\b", RegexOptions.IgnoreCase)]
    private static partial Regex SourceRegex();

    [GeneratedRegex(@"(cdrom:|[\w\-]+[/\\][\w\-%/\\.]+|\.(tim|dat|gz|ovl|vol|ins|seq|sep|vab|vh|vb|str|xa|exe|psx|cnf|bin|tex|cdp|cdo|cnp|cno|car|crs|trk|msg|txt|tmd|pmd|tod|hmd|rep|usd|cc\w?)\b)", RegexOptions.IgnoreCase)]
    private static partial Regex PathRegex();

    [GeneratedRegex(@"%[-+ #0]*\d*(\.\d+)?(l|h)?[diouxXcsfgeEp]")]
    private static partial Regex FormatRegex();

    [GeneratedRegex(@"assert|error|fail|warning|abort|cannot|can't|invalid|illegal|timeout|time out|overflow|not found|no memory|out of memory|panic|fatal|bad |unknown|unsupported|debug|\bbug\b", RegexOptions.IgnoreCase)]
    private static partial Regex DebugRegex();

    [GeneratedRegex(@"^(_*[A-Za-z][A-Za-z0-9]*(_[A-Za-z0-9]+)+|[a-z]+([A-Z][a-z0-9]+)+|[A-Z][a-z0-9]+([A-Z][a-z0-9]+)+|__\d+\w+|_\$_\w+|_vt[.$]\w+|\w+__F?\w*\d+\w*)$")]
    private static partial Regex IdentRegex();
}
