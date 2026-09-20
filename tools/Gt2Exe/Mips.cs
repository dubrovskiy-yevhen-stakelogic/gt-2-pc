namespace Gt2Exe;

/// <summary>Minimal MIPS R3000 instruction helpers (pattern matching only, no full disassembler).</summary>
public static class Mips
{
    public const uint JrRa = 0x03E00008;
    public const uint Nop = 0;

    public static uint Op(uint w) => w >> 26;
    public static uint Rs(uint w) => (w >> 21) & 31;
    public static uint Rt(uint w) => (w >> 16) & 31;
    public static short Imm(uint w) => (short)(w & 0xFFFF);

    /// <summary>addiu sp,sp,-X (function prologue).</summary>
    public static bool IsPrologue(uint w) => (w & 0xFFFF8000) == 0x27BD8000;
    /// <summary>addiu sp,sp,+X (stack release, X > 0).</summary>
    public static bool IsStackRelease(uint w) => (w & 0xFFFF8000) == 0x27BD0000 && (w & 0xFFFF) != 0;

    public static bool IsJal(uint w) => Op(w) == 3;
    public static bool IsJ(uint w) => Op(w) == 2;
    public static uint JumpTarget(uint w, uint pc) => ((pc + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2);

    public static bool IsLui(uint w) => Op(w) == 0x0F;
    public static bool IsCop2(uint w) => Op(w) == 0x12;
    /// <summary>GTE command (cop2 with CO bit set); low 6 bits select the operation.</summary>
    public static bool IsGteCommand(uint w) => IsCop2(w) && (w & 0x02000000) != 0;
    public static bool IsLwc2OrSwc2(uint w) => Op(w) is 0x32 or 0x3A;

    /// <summary>Is the opcode one that the R3000 actually implements (coarse sanity check)?</summary>
    public static bool IsPlausible(uint w)
    {
        uint op = Op(w);
        return op switch
        {
            0 => (w & 0x3F) is 0 or 2 or 3 or 4 or 6 or 7 or 8 or 9 or 0x0C or 0x0D
                 or 0x10 or 0x11 or 0x12 or 0x13 or 0x18 or 0x19 or 0x1A or 0x1B
                 or (>= 0x20 and <= 0x27) or 0x2A or 0x2B,
            1 => Rt(w) is 0 or 1 or 0x10 or 0x11,
            >= 2 and <= 0x0F => true,
            0x10 or 0x12 => true,
            >= 0x20 and <= 0x26 => true,
            >= 0x28 and <= 0x2B => true,
            0x2E or 0x32 or 0x3A => true,
            _ => false,
        };
    }

    public static readonly Dictionary<uint, string> GteCommandNames = new()
    {
        [0x01] = "RTPS", [0x06] = "NCLIP", [0x0C] = "OP", [0x10] = "DPCS", [0x11] = "INTPL",
        [0x12] = "MVMVA", [0x13] = "NCDS", [0x14] = "CDP", [0x16] = "NCDT", [0x1B] = "NCCS",
        [0x1C] = "CC", [0x1E] = "NCS", [0x20] = "NCT", [0x28] = "SQR", [0x29] = "DCPL",
        [0x2A] = "DPCT", [0x2D] = "AVSZ3", [0x2E] = "AVSZ4", [0x30] = "RTPT", [0x3D] = "GPF",
        [0x3E] = "GPL", [0x3F] = "NCCT",
    };

    /// <summary>
    /// Mask out the parts of an instruction that change when code or data is relocated
    /// (jump targets, lui immediates, and 16-bit immediates of non-sp-relative I-type ops).
    /// </summary>
    public static uint MaskRelocatable(uint w)
    {
        uint op = Op(w);
        if (op is 2 or 3) return w & 0xFC000000;
        if (op == 0x0F) return w & 0xFFFF0000;
        bool immOp = op is 9 or 0x0D || (op >= 0x20 && op <= 0x26) || (op >= 0x28 && op <= 0x2B);
        if (immOp && Rs(w) != 29 && Rs(w) != 0) return w & 0xFFFF0000;
        return w;
    }
}
