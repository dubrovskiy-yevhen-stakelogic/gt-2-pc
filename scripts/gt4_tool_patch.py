"""Small, checked adapter for the pinned PDTools GT4 VIF reader.

Some GT4 packets split positions into signed 16/8-bit delta UNPACKs.
The upstream reader only consumes the first UNPACK, losing vertices and
shifting every later face/UV in that shape. Keep the patch source reviewable.
"""
from pathlib import Path

REVISION = 11


def patch_source(source):
    root = Path(source) / 'PDTools/PDTools.Files/Models/PS2'
    external = root / 'RenderCommands/Cmd_pgluTexTableFromExternalTexSetByte.cs'
    text = external.read_text(encoding='utf-8-sig')
    text = text.replace('bs.WriteByte(ExternalTexSetIndex);', '__GT2_WRITE__')
    text = text.replace('ExternalTexSetIndex = bs.Read1Byte();', 'bs.WriteByte(ExternalTexSetIndex);')
    text = text.replace('__GT2_WRITE__', 'ExternalTexSetIndex = bs.Read1Byte();')
    external.write_text(text, encoding='utf8')
    command = root / 'VIFCommand.cs'
    text = command.read_text(encoding='utf-8-sig')
    old = 'bs.Position += 4 * 4;'
    if old not in text:
        raise ValueError('Unexpected pinned VIF command source')
    text = text.replace(old, 'UnpackData.Add(bs.ReadInt32s(4));')
    text = text.replace('else if (((int)CommandOpcode',
                        'else if (CommandOpcode == VIFCommandOpcode.STMASK) { bs.Position += 4; }\n                else if (((int)CommandOpcode', 1)
    command.write_text(text, encoding='utf8')
    shape = root / 'PGLUshape.cs'
    text = shape.read_text(encoding='utf-8-sig')
    text = text.replace('e.UnpackData.Any(a => a is int[])) ||  // Regular',
                        'e.UnpackData.Any(a => a is int[] a2 && a2.Length == 2)) ||  // Regular')
    text = text.replace('e.UnpackData.Any(a => a is short[])))',
                        'e.UnpackData.Any(a => a is short[] a2 && a2.Length == 2)))')
    start = text.index('            for (var l = 0; l < vertCommand.UnpackData.Count; l++)')
    end = text.index('            if (uvCommand is not null)', start)
    text = text[:start] + '''            var packetVertices = GT2VifPositions.Decode(packet);
            shapeData.Vertices.AddRange(packetVertices);

''' + text[end:]
    text = text.replace('for (var l = 0; l < shapeData.Vertices.Count; l++)',
                        'for (var l = 0; l < packetVertices.Count - 2; l++)')
    text = text.replace('packetFaceStart += vertCommand.Num;', 'packetFaceStart += packetVertices.Count;')
    # OBJ faces use one shared position/UV index. A shape can mix untextured
    # and textured packets (SLK dashboard, S2000 cabin, Beat instruments).
    # Missing UV rows must keep their slots, or every later packet samples
    # unrelated texture coordinates, including those of the next shape.
    start = text.index('            if (uvCommand is not null)')
    end = text.index('            if (normalsCommand is not null)', start)
    text = text[:start] + '''            var packetUvs = GT2VifPositions.DecodeUvs(packet);
            shapeData.UVs.AddRange(packetUvs);
            if (packetUvs.Count == 0)
                for (int k = 0; k < packetVertices.Count; ++k) shapeData.UVs.Add(Vector2.Zero);
            if (shapeData.UVs.Count != shapeData.Vertices.Count)
                throw new InvalidOperationException($"GT4 UV/position packet count mismatch: packet {j}, UV {shapeData.UVs.Count}, positions {shapeData.Vertices.Count}");

''' + text[end:]
    start = text.index('            if (normalsCommand is not null)')
    end = text.index('            if (vertColors is not null)', start)
    text = text[:start] + '''            var packetNormals = GT2VifPositions.DecodeNormals(packet);
            shapeData.Normals.AddRange(packetNormals);
            if (packetNormals.Count == 0)
                for (int k = 0; k < packetVertices.Count; ++k) shapeData.Normals.Add(Vector3.Zero);
            if (shapeData.Normals.Count != shapeData.Vertices.Count)
                throw new InvalidOperationException("GT4 normal/position packet count mismatch");

''' + text[end:]
    shape.write_text(text, encoding='utf8')
    model = root / 'ModelSet/ModelSetPS2Base.cs'
    text = model.read_text(encoding='utf-8-sig')
    # Some models define only an off branch for the tail-lamp callback.
    # Preserve every supplied branch without indexing an absent on branch.
    for branch in (0, 1):
        old = f'ProcessCommands(extractor, callbackCmd.CommandsPerBranch[{branch}]);'
        if text.count(old) != 1:
            raise ValueError('Unexpected pinned tail-lamp callback source')
        text = text.replace(old, f'if (callbackCmd.CommandsPerBranch.Count > {branch})\n                            ' + old)
    marker = '                    extractor.ExtraShapeName = null;'
    if text.count(marker) != 1:
        raise ValueError('Unexpected pinned model callback source')
    text = text.replace(marker, '''                    // Daytime body parts (hood, glass frame) are conditional
                    // geometry, not optional reflection effects. Select one
                    // branch, never concatenate day and night alternatives.
                    if (callbackCmd.Parameter == ModelCallbackParameter.GetTimeZone)
                    {
                        ProcessCommands(extractor, callbackCmd.Default);
                        if (callbackCmd.CommandsPerBranch.Count > 0)
                            ProcessCommands(extractor, callbackCmd.CommandsPerBranch[0]);
                    }

''' + marker)
    model.write_text(text, encoding='utf8')
    (root / 'GT2VifPositions.cs').write_text(Path(__file__).with_name('gt4_vif_positions.cs').read_text(), encoding='utf8')
    dumper = Path(source) / 'GTPS2ModelTool/Dumper.cs'
    text = dumper.read_text(encoding='utf-8-sig')
    marker = '            DumpModelSetTextures(modelSet, modelSetConfig);'
    if text.count(marker) != 1: raise ValueError('Unexpected pinned texture dumper source')
    text = text.replace(marker, marker + '''
            // PNGs are cropped to CLAMP.MAX; OBJ UVs retain TEX0 dimensions.
            var gt2Addressing = new Dictionary<string, object>();
            var gt2TextureSets = modelSet.GetTextureSetList();
            for (int i = 0; i < gt2TextureSets.Count; ++i)
            for (int j = 0; j < gt2TextureSets[i].pgluTextures.Count; ++j) {
                var t = gt2TextureSets[i].pgluTextures[j]; var c = t.ClampSettings;
                gt2Addressing[$"{i}.{j}.png"] = new {
                    width = 1 << t.tex0.TW_TextureWidth, height = 1 << t.tex0.TH_TextureHeight,
                    minU = c.MINU, maxU = c.MAXU, minV = c.MINV, maxV = c.MAXV,
                    wrapU = c.WMS.ToString(), wrapV = c.WMT.ToString()
                };
            }
            File.WriteAllText(Path.Combine(dir, "texture_addressing.json"),
                System.Text.Json.JsonSerializer.Serialize(gt2Addressing));
''')
    dumper.write_text(text, encoding='utf8')
