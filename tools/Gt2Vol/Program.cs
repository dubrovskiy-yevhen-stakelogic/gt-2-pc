using System.Globalization;

namespace Gt2Vol;

internal static class Program
{
    private static int Main(string[] args)
    {
        CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
        CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;

        if (args.Length < 2)
        {
            PrintUsage();
            return 1;
        }

        try
        {
            string command = args[0].ToLowerInvariant();
            switch (command)
            {
                case "info": return Commands.Info(args[1]);
                case "toc": return Commands.Toc(args[1]);
                case "list": return Commands.List(args[1]);
                case "extract":
                    if (args.Length < 3) break;
                    return Commands.Extract(args[1], args[2], decompress: !args.Contains("--no-gunzip"));
                case "inventory":
                    if (args.Length < 3) break;
                    return Inventory.Run(args[1], args[2]);
                case "diff":
                    if (args.Length < 4) break;
                    return VolDiff.Run(args[1], args[2], args[3]);
                case "split":
                    if (args.Length < 4) break;
                    return IdxSplit.Run(args[1], args[2], args[3]);
            }
            PrintUsage();
            return 1;
        }
        catch (Exception ex) when (ex is IOException or InvalidDataException or UnauthorizedAccessException)
        {
            Console.Error.WriteLine("error: " + ex.Message);
            return 2;
        }
    }

    private static void PrintUsage()
    {
        Console.WriteLine("""
            Gt2Vol - Gran Turismo 2 GT2.VOL (GTFS) reader. Opens the volume read-only.

              Gt2Vol info      <GT2.VOL>                 header, counts, consistency checks
              Gt2Vol toc       <GT2.VOL>                 raw TOC records (one per line)
              Gt2Vol list      <GT2.VOL>                 resolved file list: offset, size, time, path
              Gt2Vol extract   <GT2.VOL> <outDir> [--no-gunzip]
                                                         extract all files; gzip members are also
                                                         written decompressed (".gz" stripped)
              Gt2Vol inventory <GT2.VOL> <report.md>     directory / extension / signature statistics
              Gt2Vol diff      <A.VOL> <B.VOL> <report.md>
                                                         compare two volumes by path and SHA-1
              Gt2Vol split     <file.dat> <file.idx> <outDir>
                                                         split an extracted dat+idx pair (gtmenudat,
                                                         commonpic); gzip members are decompressed
            """);
    }
}
