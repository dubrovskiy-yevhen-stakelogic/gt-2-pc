using System.Text.Json;
using PDTools.SpecDB.Core;
using PDTools.SpecDB.Core.Mapping;
using PDTools.SpecDB.Core.Formats;
using Syroot.BinaryData.Core;
using System.Buffers.Binary;

if (args.Length is < 2 or > 4) throw new ArgumentException("gt4catalog <SpecDB folder> <output.json> [locale] [--physics]");
string locale = args.Length >= 3 ? args[2] : "american";
bool physics = args.Length == 4 && args[3] == "--physics";
if (args.Length == 4 && !physics) throw new ArgumentException("Expected --physics");
if (!new[] { "american", "british", "japanese", "french", "german", "italian", "spanish", "big5", "korean" }.Contains(locale))
    throw new ArgumentException("Unsupported SpecDB locale");
using var db = new SpecDB(args[0], SpecDBFolder.GT4_US2560, false);
var tableNames = new List<string> { "VARIATION" + locale, "CAR_VARIATION_" + locale, "CAR_NAME_" + locale };
if (physics) tableNames.AddRange(new[] { "GENERIC_CAR", "DEFAULT_PARTS", "DEFAULT_PARAM", "CHASSIS", "ENGINE", "GEAR", "DRIVETRAIN", "RACINGMODIFY", "BRAKE", "SUSPENSION", "FRONTTIRE", "REARTIRE", "TIRESIZE", "TURBINEKIT" });
foreach (var tableName in tableNames)
{
    var table = new Table(tableName);
    // GT4 row count and row size are 32-bit. The pinned model tool's older
    // SpecDB helper reads these as 16-bit; initialize this header explicitly.
    byte[] bytes = File.ReadAllBytes(Path.Combine(args[0], tableName + ".dbt"));
    int ReadInt(int offset) => BinaryPrimitives.ReadInt32LittleEndian(bytes.AsSpan(offset, 4));
    if (System.Text.Encoding.ASCII.GetString(bytes, 0, 4) != "GTDB" || bytes[5] != 0)
        throw new InvalidDataException("Only little-endian GT4 SpecDB is supported");
    var data = new DBT_DatabaseTable(bytes, Endian.Little) {
        TableAttribute = BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(4, 2)),
        AlignedBytes = BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(6, 2)),
        RowCount = checked((uint)ReadInt(8)), RowSize = ReadInt(12)
    };
    int book = checked(16 + (int)data.RowCount * 8);
    data.codeBookInfoBlockAdr = book;
    if ((data.TableAttribute & 1) != 0) {
        data.dataListBlockAdr = book + 8;
        data.codeListBlockAdr = book + 0x208;
        int template = checked(book + ReadInt(book));
        data.templateInfoBlockAdr = template;
        data.templateDataBlockAdr = template + 8;
        data.dataBlockAdr = checked(template + ReadInt(template));
    } else data.dataBlockAdr = book;
    table.DatabaseTable = data;
    table.ReadIDIMapOffsets(db);
    db.Tables.Add(tableName, table);
}
var variation = db.Tables["VARIATION" + locale];
variation.LoadAllRows(db);
var cars = db.Tables["CAR_VARIATION_" + locale];
cars.LoadAllRows(db);
var names = db.Tables["CAR_NAME_" + locale];
names.LoadAllRows(db);
var output = new List<object>();
foreach (var car in cars.Rows)
{
    int id = int.Parse(car.ColumnData[0].ToString());
    var variants = variation.Rows.Where(v => v.ID == id).ToList();
    var name = names.Rows.FirstOrDefault(n => n.Label == car.Label);
    output.Add(new {
        label = car.Label,
        name = name?.ColumnData.Select(c => c.ToString()).ToArray(),
        models = variants.Select(v => new {
            code = v.ColumnData[0].ToString(),
            order = v.ColumnData[1].ToString(),
            colorPatch = v.ColumnData[2].ToString(),
            color = v.ColumnData[3].ToString()
        }).ToArray()
    });
}
File.WriteAllText(args[1], JsonSerializer.Serialize(output, new JsonSerializerOptions { WriteIndented = true }));
File.WriteAllText(Path.ChangeExtension(args[1], ".variations.json"), JsonSerializer.Serialize(
    variation.Rows.Select(v => new { id = v.ID, label = v.Label,
        columns = v.ColumnData.Select(c => c.ToString()).ToArray() }),
    new JsonSerializerOptions { WriteIndented = true }));
Console.WriteLine($"Exported {output.Count} cars");
if (physics) {
    var tables = new Dictionary<string,object>();
    foreach (var tableName in tableNames.Skip(3)) {
        var table = db.Tables[tableName];
        table.LoadAllRows(db);
        tables[tableName] = new {
            rowSize = table.DatabaseTable.RowSize,
            columns = table.TableMetadata.Columns.Select(c => c.ColumnName).ToArray(),
            rows = table.Rows.Select(r => new { id = r.ID, label = r.Label,
                values = r.ColumnData.Select(c => c.ToString()).ToArray() }).ToArray()
        };
        Console.WriteLine($"{tableName}: {table.Rows.Count} rows, mapped={table.IsTableProperlyMapped}");
        if (!table.IsTableProperlyMapped) throw new InvalidDataException("Incomplete mapping: " + tableName);
    }
    File.WriteAllText(Path.ChangeExtension(args[1], ".physics.json"), JsonSerializer.Serialize(tables,
        new JsonSerializerOptions { WriteIndented = true }));
}
