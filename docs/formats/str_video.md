# Movies: STREAM.DAT, the frame bitstream, the MDEC and the players

Status 2026-09-19. Addresses are US Arcade v1.1 (SCUS_944.55 SHA-1 231f9dba7191b9ef915621662afdc40a7c66df95; GT2.OVL member 5
c40ec257e8e9b163394827a87599046aa6e3fc7c = the movie overlay, member 2 = the arcade menus) unless marked Sim (US Simulation v1.2,
SCUS_944.88 SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a). Evidence: objdump of the EXE and of the inflated members
(`work\ovl\arcade_us11\ovl5.bin`, `ovl2.bin`, `ovl1.bin`), the sectors of the user's disc image, and runs of the original in our
interpreter with the MDEC device of this pass (`gt2run movie-check`, section 7). Code: `src/gt2formats/str_video.*` (sectors,
tables, frame decoder, MDEC model, reader), `src/machine/mdec.*` (the interpreter's MDEC), `src/gt2view/movie_view.*`,
`tools/gt2game/movie_player.*`, `tools/gt2tool/movie_cmds.cpp`, `tools/gt2run/movie_check.cpp`. Facts: `db/arcade_us11_symbols.yaml`
(block "movies").

## 1. Which disc plays what

- **Simulation disc: no movies.** There is no STREAM.DAT in its root, and the EXE's main (Sim 0x8005D6E0) enters member 1 (the
  title) directly (`jal 0x8005DA3C` with a0 = 1 at Sim 0x8005D700); no code of the Simulation disc selects member 5 (jal scan of
  the EXE and all members, `docs/research/menus_gtmode.md` 2.1). The EXE still links libpress and names "stream.dat" (scout_disc.md).
  (hardware_boundary.md attributes the Simulation boot's black screen up to ~field 900 to "the intro FMV": no movie is involved
  there - no code path of that disc reaches member 5 or a STREAM.DAT.)
- **Arcade disc:** 27 movies in STREAM.DAT (LBA 146399 on the US v1.1 image):

| Movie | Frames | Size | Rate | Audio | Played by |
|---|---|---|---|---|---|
| 0..23 | 419 each | 112 x 96 | 2 fields per frame | XA present, every sample 0 | COURSE SELECTION preview (member 2), 15-bit, looping |
| 24 | 4649 | 320 x 192 | 2 fields | 155 s stereo | intro: boot (EXE main 0x8005D670 -> member 5) and every 4th attract cycle; Start skips |
| 25 | 3934 | 640 x 216 | 4 fields | 262 s | ending, ENDING CREDITS row 0 (Arcade Mode) |
| 26 | 4999 | 640 x 224 | 4 fields | 333 s | ending, ENDING CREDITS row 1 (Simulation Mode) |

The course -> preview table is member 2's 0x80053334 (u16, indexed by the s16 at +6 of the course's map picture record):
0 0 1 2 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 19 20 20 16 21 22 23 0. `gt2tool str-info <arcade.bin> STREAM.DAT` lists all.

## 2. STREAM.DAT

Mode 2 sectors, file number 1. Data sectors (Form 1, submode 0x48) of **any** channel 0..6 carry video; channel 7 carries XA-ADPCM
(stereo, 37800 Hz, 4 bit; one sector in eight = the double-speed audio rate). Padding data sectors of the unused channels have a
zero header and are ignored by the magic test. Data sector = 32-byte header + 2016 data bytes:

| Offset | Type | Meaning (0x800106EC reads 8 words, then 504 words) |
|---|---|---|
| +0x00 | u16 0x0160, u16 0x5349 | magic ("IS"); both checked |
| +0x04 | u16 | chunk index |
| +0x06 | u16 | chunks in the frame (at most 8 on the disc) |
| +0x08 | u32 | frame number, 1-based |
| +0x0C | u32 | used bytes (not read) |
| +0x10 | u32 | frame count in the low half; flags 0x8000 / 0x4000 / 0xC000 (first / last chunk) in the high half, masked off with 0x3FFFFFFF |

Movie table EXE 0x80092088 (28 u32 sector offsets): movie i = offsets [t[i], t[i] + t[i+1] - t[i] - 25) (0x800100A4 -> 0x800100F8;
the 25 sectors in between are not requested; the drive's position poll hands over at most one or two of them, see 7).
0x800100F8 fills the CD stream block 0x801EFF00: +0x18 volume 0x8000, +0x21 mode 0xC0 (double speed, XA), +0x22 / +0x23 filter
file 1 channel 7, +0x24 / +0x28 first / last LBA (base = STREAM.DAT's LBA kept in 0x801D8E2C), +0x2C the sector callback.

**Frame assembly (sector callback 0x800106EC):** a chunk 0 starts a frame in the next free slot of a 32-slot ring (0x80010D38:
slots of 0x46E0 bytes; a full ring drops the frame, flag 2); later chunks must belong to the same frame and arrive in order,
otherwise the frame is dropped; the last chunk completes it; a completed frame numbered >= the frame count ends the stream (flag 4).
`GtMovieReader` reproduces these rules (the ring's capacity is applied by the player). On the disc every frame of every movie is
complete and in order.

## 3. The frame bitstream (member 5 0x80010AC0; member 2 0x80026340 is the same code)

    u16 words      MDEC parameter words of the frame (a multiple of 32 on the whole disc)
    u16 0x3800     the top half of the MDEC decode command
    u16 width, u16 height
    u16 zipBytes   GT-ZIP data length
    GT-ZIP data    EXE 0x80083C1C (gt2formats/course_map.h InflateGtZip) -> mb * 12 bytes: for the mb * 6 blocks
                   (Cr, Cb, Y0..Y3 per macroblock) first all high bytes, then all low bytes of the block's first MDEC code
                   (quantiser scale << 10 | DC)
    AC bitstream   16-bit little-endian words read most significant bit first

The decoder keeps a 32-bit window (a0) with a1 = the valid bits beyond 16; consuming n bits shifts the window and, when a1 <= 0,
ORs the next word in at 16 - a1 (a1 += 16). Per block: the DC word, then codes until end of block:
- top bit 1: "10" = end of block (0xFE00, 2 bits); "110" / "111" = run 0 level +1 / -1 (0x0001 / 0x03FF, 3 bits);
- otherwise z = leading zeros (the GTE's LZCS / LZCR): z >= 9 (top 9 bits zero) skips 9 bits first; z == 5 is the escape
  (6 bits, then 16 bits run << 10 | level verbatim); else the table at member 5 0x800114CC + (z << 8) + ((window >> (23 - z)) &
  0xFC) (after the 9-bit skip: z - 9) gives u32 = length << 26 | MDEC code. z = 1..11 are used (0x800115CC..0x800120CC; member 2
  keeps an identical copy at 0x80052734 + 0x100..). The codes are MPEG-1's AC table; the table is read from the disc.
After the last block the output is padded with 0xFE00 up to 4 + 4 * words bytes. Port: `GtFrameToMdecCodes`.

## 4. The players

**Member 5** (entry 0x800114B8 = boot: 0x80011328 then member 1; entry 0x800114E0(a0): a0 == 0 -> 0x800113A8, else 0x80011430,
then member 1). The three set-ups call 0x80010E64(obj, movie, flag), clear VRAM (0x80010DA8), set the display mode (0x8007D000:
49 = 320 x 240 24-bit for the intro, 19 / 51 = 640 x 240 24-bit for the endings) and run the view 0x80011570:

| | intro 0x80011328 | ending 0x80011430 | ending 0x800113A8 |
|---|---|---|---|
| movie | 24 | 25 | 26 |
| display buffers (+0x1E), positions | 3: (0,0) (320,0) (0,256) (0x800120D4) | 2: (0,0) (0,256) (0x800120CC) | same |
| picture offset (+0x150, +0x154) | (0, 24) | (0, 12) | (0, 8) |
| fields per frame (+0x18) | 2 | 4 | 4 |
| frames buffered before the first is shown (+0x13C) | 2 | 0 | 0 |
| skip (+0xE0) | Start: generic pad bit 0x10000 of +0xD4 (0x8001102C) | no | no |

All movies are decoded to **24-bit** (0x80010000 -> 0x80010950 with +0xE4 = 1: DecDCTin(buf, 1)): 0x800109E4 starts DecDCTin on
the frame's codes and DecDCTout of one 16-pixel column (16 x ((h + 15) & ~15) pixels = 12 * h' words); the DMA1 callback 0x800104C4
LoadImages the column (x in halfwords: x * 3 / 2, 24 wide, the frame's height, so the rows below it are not loaded) and starts the
next. 0x800111D4 (every field) shows the next decoded buffer every +0x18 fields (0x80081E3C(x * 3 / 2, y)). A stalled stream ends
the movie after 900 fields (0x800110C0).

**Member 2, COURSE SELECTION:** 0x800228A4 (first course / every change: index = s16 at the map picture record + 6, countdown 12);
0x80022EBC decrements it while the view is not leaving and starts the preview object 0x80129510 at 0 (0x80013ADC); 0x80013B0C stops
it (course change, leaving). The object (0x8001805C per field, 0x800265B8 open: 0x80025880 with +0xE4 = 0 -> 15-bit) loads each
frame at VRAM (640, 256) (0x80025B98) and restarts the same sectors at the end; 0x80013B34 -> 0x8001819C draws a 112 x 96 SPRT
tpage 0x11A (15-bit, (640, 256)), colour 0x808080, at (24, 254) into OT slot 0, once a frame is decoded.

**Title attract (member 1):** result 6 -> 0x8001156C: 0x801EF030 = (+1) mod 4; 0 -> member 5 (the intro), otherwise the attract race
(0x80020A50). **ENDING CREDITS:** a row -> view 0x80052670 (24 fields, draws nothing) -> result 5 (row 0) / 6 (row 1) -> ovl2 top
level 0x80011804 / 0x80011828 -> member 5 0x800114E0(1 / 0).

**Audio:** the CD stream plays XA file 1 channel 7 through the SPU's CD input at volume 0x8000 with the stream script's fade-in
(0x2000 per field, as the race music, `sound.md` 6). The previews' channel 7 is silence.

## 5. The MDEC and its model

Register interface (psx-spx): 0x1F801820 write = command / parameter, read = data out; 0x1F801824 write = control (bit 31 reset,
bit 30 / 29 enable the DMA0 / DMA1 requests), read = status (31 out FIFO empty, 29 busy, 28 / 27 data-in / out request, 26..23 the
command's depth / signed / bit 15, 18..16 current block, 15..0 remaining parameter words - 1). Commands: 1 decode (bits 28..27 depth
0 = 4-bit, 1 = 8-bit, 2 = 24-bit, 3 = 15-bit; 26 signed; 25 bit 15; 15..0 words), 2 quant tables (bit 0: luma + chroma), 3 scale
table. libpress as linked in the EXE: DecDCTReset 0x80086568 (mode 0 sends the tables of 0x800A721C = 0x40000001 + 128 bytes and
0x800A72A0 = 0x60000000 + 64 s16), DecDCTin 0x800866C0 (mode bit 0 clears command bit 27 = 24-bit, bit 1 sets bit 25),
DecDCTinRaw 0x80086918 (first word written directly, the rest by DMA0 in (words >> 5) blocks of 32, CHCR 0x01000201), DecDCTout
0x8008673C -> 0x800869A8 (DMA1, CHCR 0x01000200), callbacks 0x80086804 (DMA1) / 0x800867E0 (DMA0), syncs poll status bit 29 / DMA1
CHCR bit 24.

The decode model (`MdecCore`, used by the native player **and** the interpreter's device - one implementation):
1. Block: leading 0xFE00 words skipped; first word = q << 10 | DC; DC value = s10(DC) * quant[0]; AC k += run + 1, value =
   (s10(level) * quant[k] * q + 4) >> 3; q == 0: value = s10 * 2 stored at raster position k, else at zigzag[k]; every value clamped
   to -0x400..0x3FF; the block ends when k > 63 (0xFE00 = run 63). Chroma blocks use the second quant table.
2. IDCT: two passes of out[x + y * 8] = (sum over z of in[y + z * 8] * (scale[x + z * 8] >> 3) + 0xFFF) >> 13 (arithmetic
   shifts), the second pass on the first's output (psx-spx "real_idct_core").
3. Colour: for each pixel, Cr / Cb of its 2 x 2 cell, R = Y + ((359 Cr + 128) >> 8), G = Y + ((-88 Cb - 183 Cr + 128) >> 8),
   B = Y + ((454 Cb + 128) >> 8), each clamped to -128..127, + 128 when unsigned; 24-bit = R, G, B bytes (16 x 16 pixels = 192
   words per macroblock), 15-bit = each byte >> 3 (+ bit 15), monochrome = Y clamped (8-bit / 4-bit = byte >> 4).
Evidence and limits: the steps and constants 1..3 follow psx-spx's MDEC description (its IDCT rounding is marked "or so" there; the
colour multipliers 1.402 / -0.3437 / -0.7143 / 1.772 are taken as the nearest x / 256). No console capture exists here, so the model
is **not proven bit-exact to real hardware**; what is proven is that the original's own pipeline (its demux, GT-ZIP, VLC decoder,
libpress, DMA, column LoadImages and display flips, all running as MIPS code in the interpreter) produces exactly the pictures the
native pipeline produces with the same model (section 7). Replacing the model changes both sides at once.

Interpreter device (`src/machine/mdec.*`): a decode command decodes as its parameter words arrive (DMA0 is instantaneous), whole
macroblocks go to the output FIFO, DMA1 / data reads pop it; the command ends when its parameter count is reached (an unfinished
block is dropped). DMA channels 0 / 1 in `Machine::RunDma` (sync modes 0 / 1). The GPU shows 24-bit display areas (`DisplayRgba`).

## 6. The native player (gt2game, Arcade disc)

- Boot: the intro before the title (skippable with Start / S / Esc); every fourth title attract cycle plays it again, the other three
  play the next demo file replay (docs/formats/title.md section 11).
- ENDING CREDITS: the chosen row's ending after 24 fields (the credits page stays drawn meanwhile - ours; the original's view is
  empty), not skippable, then the title.
- COURSE SELECTION: the preview (CoursePreview on the course page's hooks, `arcade_course_page.h` CourseMovieSource), looping.
- Pacing: the stream advances 150 * 1001 / 60000 sectors per field; completed frames enter a 32-frame ring; a frame is shown every
  2 / 4 fields; the audio is decoded on the sound device's thread from the same sectors (own disc handle), linear 37800 -> 44100 Hz
  like the runtime SPU, fade-in 0x2000 per field.
- Flags: `--no-movies`; `--movies` (also play the boot intro in scripted / shot runs, which skip it by default; `--arcade-compare`
  runs load no movies because their captures predate the MDEC); `--movie N` plays one movie and exits.
- Picture: the frame is an RGBA8 image in the renderer's external texture store (its last 640 x 256 texels), one nearest-texel quad
  in the display's 4:3 area. `gt2game <arcade.bin> --movie 24 --window 320x240 --fast --no-sound --shot-at 1900 x.png --script
  "1905:s"`: the screenshot's movie rectangle = `str-export` frame 950, 0 differing pixels.

## 7. Verification

`gt2run movie-check <arcade.bin> <fields> <outDir> "<gt2play script>" [poke8=field:addr:byte[:count]]` runs the original with
the MDEC device and compares every change of the displayed movie picture (24-bit movies: the display cut at the player's offset;
previews: VRAM (640, 256) 112 x 96) with the native decode of the frames after the last match (a restart of the preview's sectors
is followed), and the XA sectors the CD handed to the SPU with the native reader's (order), plus the runtime SPU's XA path (its own
ADPCM decoder, linear resampling, volumes) against `DecodeXaSector` + the same resampling on those sectors. Results (2026-09-19):

| Run (script; outputs under `work\fmv\`) | Pictures | Audio |
|---|---|---|
| boot, no input, 10500 fields (`check_intro`) | movie 24: 4563 displayed pictures, 4563 = a native frame (frames 1..4626), 0 differ | 2906 sectors (1 behind the end), 2905 in order; 13 665 730 samples, 0 differ |
| to COURSE SELECTION, no further input, 6000 fields (`check_course_loop2`) | movie 9 (Tahiti Road): 1021 pictures over two passes, 1021 equal, 0 differ | 643 sectors (2 behind the end), 641 in order over the loop; 0 differ |
| COURSE SELECTION with course changes `...,4300:right,4700:right,5100:left` (`check_course_changes`) | movies 6 / 9 / 15: 430 / 179 / 190 equal, 0 differ | 5 plays, each in order, 0 differ |
| BONUS ITEMS -> ENDING CREDITS row 1, `poke8=1900:801C9555:1`, 24000 fields (`check_end26`) | movie 26: 4863 pictures, 4863 equal (frames 1..4997), 0 differ | 6249 sectors in order, 29 391 202 samples, 0 differ |
| ENDING CREDITS row 0, `poke8=1900:801C93F8:6:64`, 20000 fields (`check_end25`) | movie 25: 3861 pictures, 3861 equal (frames 1..3932), 0 differ | 4918 sectors in order, 23 130 178 samples, 0 differ |

Script to the menus: `600:start,700:cross,900:start,1100:start,1300:cross`; COURSE SELECTION: `...,2000:cross,2400:cross,2700:cross,
3000:cross,3300:cross,3600:cross,3900:cross`; ENDING CREDITS row 1: `...,2000:down,2100:down,2200:cross,2500:cross,2800:down,
2900:cross`. The pokes are dev capture aids (career + 0x215 = the Simulation ending open; career + 0xB8.. flag bytes = the Arcade
ending open), as `gt2play --poke`.
MDEC totals of the intro run: 4650 decode commands, 1 115 520 macroblocks, 0 reads past the output FIFO.

## 8. Open

- The MDEC model against real hardware (IDCT rounding / the scale table's >> 3, the colour multipliers, 15-bit rounding): needs a
  console capture (section 5).
- The preview's start latency (the preview object's states and the CD seek; ours starts the stream when the countdown ends) and the
  intro's 2-frame prebuffer are timing only; the pictures are the original's.
- (Done 2026-09-19: the arcade title's attract demos, docs/formats/title.md section 11.)
