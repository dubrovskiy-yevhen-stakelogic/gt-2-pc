using System.Buffers.Binary;
using System.Text;

namespace Gt2Disc;

/// <summary>
/// CD-ROM XA attribute bits stored (big-endian) in the system-use area of a directory record.
/// </summary>
[Flags]
public enum XaAttributes : ushort
{
    None = 0,
    OwnerRead = 0x0001,
    OwnerExecute = 0x0004,
    GroupRead = 0x0010,
    GroupExecute = 0x0040,
    WorldRead = 0x0100,
    WorldExecute = 0x0400,
    Form1 = 0x0800,
    Form2 = 0x1000,
    Interleaved = 0x2000,
    Cdda = 0x4000,
    Directory = 0x8000,
}

public sealed class IsoEntry
{
    public required string Name { get; init; }          // without ";1" version suffix
    public required string FullPath { get; init; }      // forward-slash separated, no leading slash
    public required int Lba { get; init; }
    public required uint Size { get; init; }            // as stored in the directory record
    public required byte IsoFlags { get; init; }
    public required DateTime? Recorded { get; init; }
    public required bool HasXa { get; init; }
    public required XaAttributes Xa { get; init; }
    public required byte XaFileNumber { get; init; }
    public required ushort XaGroupId { get; init; }
    public required ushort XaUserId { get; init; }
    public List<IsoEntry> Children { get; } = new();

    public bool IsDirectory => (IsoFlags & 0x02) != 0;

    /// <summary>Number of sectors occupied; directory record sizes are always in 2048-byte units.</summary>
    public int SectorCount => (int)((Size + 2047) / 2048);

    public bool XaSaysRaw => HasXa && (Xa & (XaAttributes.Form2 | XaAttributes.Interleaved | XaAttributes.Cdda)) != 0;

    public string XaFlagString()
    {
        if (!HasXa) return "-----";
        Span<char> c = stackalloc char[5];
        c[0] = (Xa & XaAttributes.Directory) != 0 ? 'D' : '-';
        c[1] = (Xa & XaAttributes.Form1) != 0 ? '1' : '-';
        c[2] = (Xa & XaAttributes.Form2) != 0 ? '2' : '-';
        c[3] = (Xa & XaAttributes.Interleaved) != 0 ? 'I' : '-';
        c[4] = (Xa & XaAttributes.Cdda) != 0 ? 'A' : '-';
        return new string(c);
    }
}

public sealed class PrimaryVolumeDescriptor
{
    public required string SystemId { get; init; }
    public required string VolumeId { get; init; }
    public required uint VolumeSpaceSize { get; init; }
    public required ushort LogicalBlockSize { get; init; }
    public required uint PathTableSize { get; init; }
    public required uint PathTableLba { get; init; }
    public required uint OptionalPathTableLba { get; init; }
    public required uint PathTableMsbLba { get; init; }
    public required uint OptionalPathTableMsbLba { get; init; }
    public required string VolumeSetId { get; init; }
    public required string PublisherId { get; init; }
    public required string DataPreparerId { get; init; }
    public required string ApplicationId { get; init; }
    public required string CopyrightFileId { get; init; }
    public required string AbstractFileId { get; init; }
    public required string BibliographicFileId { get; init; }
    public required string CreationDate { get; init; }
    public required string ModificationDate { get; init; }
    public required bool HasXaSignature { get; init; }   // "CD-XA001" at offset 1024
    public required int RootLba { get; init; }
    public required uint RootSize { get; init; }

    public IEnumerable<(int Lba, string Label)> PathTableLocations()
    {
        yield return ((int)PathTableLba, "<path table L>");
        yield return ((int)OptionalPathTableLba, "<path table L (optional copy)>");
        yield return ((int)PathTableMsbLba, "<path table M>");
        yield return ((int)OptionalPathTableMsbLba, "<path table M (optional copy)>");
    }
}

/// <summary>
/// Minimal ISO9660 + CD-XA directory reader on top of a raw Mode 2 image.
/// </summary>
public sealed class IsoVolume
{
    private static readonly Encoding Ascii = Encoding.ASCII;

    public RawDiscImage Image { get; }
    public PrimaryVolumeDescriptor Pvd { get; }
    public IsoEntry Root { get; }

    public IsoVolume(RawDiscImage image)
    {
        Image = image;
        byte[] pvd = image.ReadForm1(16, 1);
        if (pvd[0] != 1 || Ascii.GetString(pvd, 1, 5) != "CD001")
            throw new InvalidDataException("No ISO9660 primary volume descriptor at LBA 16.");

        Pvd = new PrimaryVolumeDescriptor
        {
            SystemId = Text(pvd, 8, 32),
            VolumeId = Text(pvd, 40, 32),
            VolumeSpaceSize = BinaryPrimitives.ReadUInt32LittleEndian(pvd.AsSpan(80)),
            LogicalBlockSize = BinaryPrimitives.ReadUInt16LittleEndian(pvd.AsSpan(128)),
            PathTableSize = BinaryPrimitives.ReadUInt32LittleEndian(pvd.AsSpan(132)),
            PathTableLba = BinaryPrimitives.ReadUInt32LittleEndian(pvd.AsSpan(140)),
            OptionalPathTableLba = BinaryPrimitives.ReadUInt32LittleEndian(pvd.AsSpan(144)),
            PathTableMsbLba = BinaryPrimitives.ReadUInt32BigEndian(pvd.AsSpan(148)),
            OptionalPathTableMsbLba = BinaryPrimitives.ReadUInt32BigEndian(pvd.AsSpan(152)),
            VolumeSetId = Text(pvd, 190, 128),
            PublisherId = Text(pvd, 318, 128),
            DataPreparerId = Text(pvd, 446, 128),
            ApplicationId = Text(pvd, 574, 128),
            CopyrightFileId = Text(pvd, 702, 37),
            AbstractFileId = Text(pvd, 739, 37),
            BibliographicFileId = Text(pvd, 776, 37),
            CreationDate = Text(pvd, 813, 16),
            ModificationDate = Text(pvd, 830, 16),
            HasXaSignature = Ascii.GetString(pvd, 1024, 8) == "CD-XA001",
            RootLba = (int)BinaryPrimitives.ReadUInt32LittleEndian(pvd.AsSpan(156 + 2)),
            RootSize = BinaryPrimitives.ReadUInt32LittleEndian(pvd.AsSpan(156 + 10)),
        };
        if (Pvd.LogicalBlockSize != 2048)
            throw new InvalidDataException($"Unsupported logical block size {Pvd.LogicalBlockSize}.");

        Root = ParseRecord(pvd.AsSpan(156, pvd[156]), parentPath: null, isRoot: true)
               ?? throw new InvalidDataException("Invalid root directory record.");
        ReadDirectory(Root, depth: 0);
    }

    /// <summary>All entries (directories and files) in depth-first directory order, root excluded.</summary>
    public IEnumerable<IsoEntry> Walk() => Walk(Root);

    private static IEnumerable<IsoEntry> Walk(IsoEntry dir)
    {
        foreach (var child in dir.Children)
        {
            yield return child;
            if (child.IsDirectory)
                foreach (var e in Walk(child))
                    yield return e;
        }
    }

    public IEnumerable<IsoEntry> Files() => Walk().Where(e => !e.IsDirectory);

    public IsoEntry? Find(string path)
    {
        string wanted = path.Replace('\\', '/').Trim('/');
        return Walk().FirstOrDefault(e => string.Equals(e.FullPath, wanted, StringComparison.OrdinalIgnoreCase));
    }

    private void ReadDirectory(IsoEntry dir, int depth)
    {
        if (depth > 16) throw new InvalidDataException("Directory nesting too deep (loop?).");
        byte[] data = Image.ReadForm1(dir.Lba, dir.SectorCount);
        int pos = 0;
        while (pos < data.Length)
        {
            int len = data[pos];
            if (len == 0)
            {
                // Records never cross a sector boundary; zero length means "skip to next sector".
                pos = (pos / 2048 + 1) * 2048;
                continue;
            }
            if (pos + len > data.Length) break;
            var entry = ParseRecord(data.AsSpan(pos, len), dir.FullPath, isRoot: false);
            pos += len;
            if (entry == null) continue; // "." or ".."
            dir.Children.Add(entry);
        }
        foreach (var child in dir.Children)
            if (child.IsDirectory)
                ReadDirectory(child, depth + 1);
    }

    private static IsoEntry? ParseRecord(ReadOnlySpan<byte> rec, string? parentPath, bool isRoot)
    {
        if (rec.Length < 34) return null;
        int lba = (int)BinaryPrimitives.ReadUInt32LittleEndian(rec[2..]);
        uint size = BinaryPrimitives.ReadUInt32LittleEndian(rec[10..]);
        byte flags = rec[25];
        int nameLen = rec[32];
        ReadOnlySpan<byte> nameBytes = rec.Slice(33, nameLen);

        string name;
        if (isRoot)
            name = "";
        else if (nameLen == 1 && (nameBytes[0] == 0 || nameBytes[0] == 1))
            return null;
        else
        {
            name = Ascii.GetString(nameBytes);
            int semi = name.LastIndexOf(';');
            if (semi >= 0) name = name[..semi];
        }

        // System-use area starts after the (even-padded) name.
        int su = 33 + nameLen;
        if ((nameLen & 1) == 0) su++;
        bool hasXa = false;
        XaAttributes xa = XaAttributes.None;
        byte xaFile = 0;
        ushort gid = 0, uid = 0;
        if (rec.Length >= su + 14 && rec[su + 6] == (byte)'X' && rec[su + 7] == (byte)'A')
        {
            hasXa = true;
            gid = BinaryPrimitives.ReadUInt16BigEndian(rec[su..]);
            uid = BinaryPrimitives.ReadUInt16BigEndian(rec[(su + 2)..]);
            xa = (XaAttributes)BinaryPrimitives.ReadUInt16BigEndian(rec[(su + 4)..]);
            xaFile = rec[su + 8];
        }

        DateTime? recorded = null;
        try
        {
            if (rec[19] != 0)
            {
                var local = new DateTime(1900 + rec[18], rec[19], rec[20], rec[21], rec[22], rec[23], DateTimeKind.Unspecified);
                recorded = local;
            }
        }
        catch (ArgumentOutOfRangeException) { /* keep null */ }

        string full = string.IsNullOrEmpty(parentPath) ? name : parentPath + "/" + name;
        return new IsoEntry
        {
            Name = name,
            FullPath = full,
            Lba = lba,
            Size = size,
            IsoFlags = flags,
            Recorded = recorded,
            HasXa = hasXa,
            Xa = xa,
            XaFileNumber = xaFile,
            XaGroupId = gid,
            XaUserId = uid,
        };
    }

    private static string Text(byte[] data, int offset, int length) =>
        Ascii.GetString(data, offset, length).TrimEnd(' ', '\0');
}
