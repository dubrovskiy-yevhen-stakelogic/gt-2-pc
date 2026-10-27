// Import adapter for PDTools, compiled from source by import_gt4_models.py.
using System;
using System.Collections.Generic;
using System.Numerics;
namespace PDTools.Files.Models.PS2;
public static class GT2VifPositions {
    public static List<Vector3> Decode(VIFPacket packet) {
        return DecodeBank(packet, 0, 3);
    }
    public static List<Vector2> DecodeUvs(VIFPacket packet) {
        var result = new List<Vector2>();
        foreach (var value in DecodeBank(packet, 64, 2)) result.Add(new Vector2(value.X, 1f-value.Y));
        return result;
    }
    public static List<Vector3> DecodeNormals(VIFPacket packet) {
        bool external = packet.Commands.Exists(c => (c.VUAddr & 1023)==128 &&
                         (((int)c.CommandOpcode >> 2)&3)==2);
        return DecodeBank(packet, external?128:64, 3, true);
    }
    private static List<Vector3> DecodeBank(VIFPacket packet, int bank, int components, bool normals=false) {
        var values = new Vector3[64];
        var written = new bool[64];
        var row = new int[4];
        int mode = 0, count = 0;
        // Tweened panels carry the alternate pose at +32 in both banks.
        // C0C0 is their packet marker; the default pose must not concatenate
        // with its animation target (or turn the unused middle into a hole).
        bool tween = packet.Commands.Exists(c => c.VUAddr == 0xc020 && (int)c.CommandOpcode == 0x68);
        int limit = tween ? 32 : 64;
        foreach (var cmd in packet.Commands) {
            if (cmd.CommandOpcode == VIFCommandOpcode.STMOD) { mode = cmd.VUAddr & 3; continue; }
            if (cmd.CommandOpcode == VIFCommandOpcode.STROW) { row = (int[])((int[])cmd.UnpackData[0]).Clone(); continue; }
            int op = (int)cmd.CommandOpcode, address = cmd.VUAddr & 1023;
            // Position bank, V3 only. UVs, normals, colours and strip data
            // occupy separate banks and must not advance position indices.
            if ((op & 0x60) != 0x60 || ((op >> 2) & 3) != components-1 || address < bank || address >= bank+limit) continue;
            address -= bank;
            foreach (var raw in cmd.UnpackData) {
                if (address >= 64) throw new InvalidOperationException("GT4 position bank overflow");
                var v = new float[3];
                for (int k = 0; k < components; ++k) {
                    bool unsigned = (cmd.VUAddr & 0x4000) != 0;
                    int value = raw switch {
                        int[] a => a[k],
                        short[] a => unsigned ? (ushort)a[k] : a[k],
                        byte[] a => unsigned ? a[k] : (sbyte)a[k],
                        _ => throw new InvalidOperationException("Unsupported GT4 position encoding")
                    };
                    if (mode != 0) value = unchecked(value + row[k]);
                    if (mode == 2) row[k] = value;
                    if (mode > 2) throw new InvalidOperationException("Unsupported GT4 VIF addition mode");
                    // The signed 32-bit form is fixed point too. Only the
                    // unsigned Cxxx banks carry IEEE floats (8040 can mix
                    // 32/16-bit UV values in one strip).
                    v[k] = raw is int[] && unsigned ? BitConverter.Int32BitsToSingle(value) :
                           value / (normals && raw is byte[] ? 127f : 4096f);
                }
                values[address] = new Vector3(v[0], v[1], v[2]);
                written[address++] = true; count = Math.Max(count, address);
            }
        }
        var result = new List<Vector3>(count);
        for (int i = 0; i < count; ++i) {
            if (!written[i]) throw new InvalidOperationException("Incomplete GT4 position packet");
            result.Add(values[i]);
        }
        return result;
    }
}
