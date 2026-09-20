# .carinfoa / .carinfoe / .carinfoj

Status 2026-09-18. Derived from real bytes of US Simulation v1.2 `.carinfoe` (48,012 bytes); no external
reference was used. Parser: `src/gt2formats/car_info.*`. Verified by `gt2tool car-scan`: all 1,110 cars in
`carobj/` are found and the paint ids of every entry equal the paint ids in the car's `.cdp`.

| Offset | Field |
|---|---|
| 0x00 | `"CAR\0"` |
| 0x04 | u16 count (1110), u16 0 |
| 0x08 | count x { u32 packedCarId, u16 entryOffset, u16 flags } - flags meaning unknown (0x0000, 0x0004, 0x0010, 0x0018, 0x0780 ... seen) |

Entry (at entryOffset, n = paint count of the car's `.cdp`, **not stored here**):

- `u16 chipColor[n]` - PS1 15-bit colour of the paint chip shown in menus
- `u8 paintId[n]` - same ids as `.cdp` header
- `u8 code` - unknown (0x15, 0x17, 0x18 ... ; 0x00 for nameless objects)
- pad byte 0x7F if needed so the name starts at an even offset
- NUL-terminated ASCII name, padded with 0x00 to even

21 entries have an empty name - these are non-car objects (`gt--*`, menu props).

## Packed car id

5 characters, 6 bits each, first character most significant:
`'-'` = 0, `'0'..'9'` = 1..10, `'a'..'z'` = 11..36. Example: `a-a7r` -> 0x0B00B21C, `a-emn` -> 0x0B00F5D8.
The same packed id is expected in the GTDT car parameter tables (not verified yet).

## Open

`flags`, `code`; differences between the a/e/j variants; `.carcolor` (paint id -> colour name).
