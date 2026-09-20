# Scout 1 - извлечение дисков Gran Turismo 2 (PS1)

Дата: 2026-09-17. Все факты ниже проверены на реальных образах инструментом `tools\Gt2Disc`
(плюс ad-hoc PowerShell для hex-просмотра). Догадки помечены словом **ДОГАДКА**.
Образы открывались только на чтение; все извлечённые данные лежат только под `work\`.

## 1. Инструмент `tools\Gt2Disc`

C# console, `net8.0`, без NuGet. Сборка: `dotnet build tools\Gt2Disc\Gt2Disc.csproj -c Release`
(0 warnings / 0 errors). Исполняемый файл: `tools\Gt2Disc\bin\Release\net8.0\Gt2Disc.exe`.

| Файл | Назначение |
|---|---|
| `RawDiscImage.cs` | read-only доступ к raw-образу 2352 байт/сектор, проверка sync, разбор header + XA subheader (`Submode` flags) |
| `Iso9660.cs` | PVD (LBA 16), directory records, CD-XA system-use entry (`XaAttributes`, file number), path table LBA |
| `FileReader.cs` | скан subheader всех секторов файла (`SectorScan`), cooked-копия Form1 (2048 @ offset 24), raw-копия 2352, SHA-1 |
| `PsxExe.cs` | разбор заголовка PS-X EXE |
| `Program.cs` | команды CLI |

Команды:

```
Gt2Disc info    <image.bin>                      PVD + license sector
Gt2Disc list    <image.bin> [--scan]             дерево: LBA, size, sectors, XA attr, file number, дата; --scan = скан submode каждого сектора
Gt2Disc map     <image.bin>                      extents по LBA, включая "дыры", на которые не ссылается ни одна directory record
Gt2Disc extract <image.bin> <outdir> [--only a,b]
Gt2Disc hash    <image.bin>                      SHA-1 каждого файла (TSV)
Gt2Disc compare <imageA.bin> <imageB.bin>        сравнение двух образов по SHA-1
Gt2Disc exeinfo <image.bin> [<path>]             SYSTEM.CNF + заголовок boot EXE
```

Правило extract: файл пишется как raw 2352-байтные сектора с суффиксом `.raw2352`, если
XA attributes содержат Form2 (0x1000) / Interleaved (0x2000) / CDDA (0x4000), **или** если скан
subheader находит хотя бы один сектор с битом Form2 (0x20), не-Mode2 сектор или сектор без sync.
Иначе - обычный файл из 2048-байтных user data, обрезанный до размера из directory record.

Важная деталь `compare`/`hash`: SHA-1 raw-файлов считается двумя способами. "Как извлечён" (все 2352
байта) зависит от LBA, потому что в header сектора лежит адрес MSF. Поэтому для сравнения дисков
используется location-independent hash по байтам 16..2351 каждого сектора (subheader + data + EDC/ECC;
в Mode 2 они от адреса не зависят).

Проверка корректности: SHA-1 всех 14 извлечённых файлов, посчитанные независимо через
`Get-FileHash`, совпали с выводом `Gt2Disc hash`.

Сохранённые листинги: `work\listings\` (`*_list.txt`, `*_map.txt`, `*_exeinfo.txt`, `*_sha1.tsv`, `compare_*.txt`).

## 2. Образы и PVD

| | USA Simulation v1.2 | USA Arcade v1.1 | JP Disc 1 Arcade |
|---|---|---|---|
| Размер образа | 691,850,208 = 294,154 секторов | 729,423,408 = 310,129 секторов | 725,234,496 = 308,348 секторов |
| Volume space size (PVD) | 294,154 (совпадает) | 310,129 (совпадает) | 308,348 (совпадает) |
| System ID / Volume ID | `PLAYSTATION` / `GRANTURISMO2` | то же | то же |
| Publisher | `SONY COMPUTER ENTERTAINMENT AMERICA` | то же | `SONY COMPUTER ENTERTAINMENT INC.` |
| Data preparer | `POLYPHONY DIGITAL INC.` | то же | то же |
| PVD creation date | `1999121000000000` | `1999121000000000` | `1999112907200200` |
| `CD-XA001` signature | да | да | да |
| License sector (LBA 4) | `Licensed by Sony Computer Entertainment America` | то же | `Licensed by Sony Computer Entertainment Inc.` |
| Path table / root dir | LBA 18 (10 байт) / LBA 22 (2048 байт) | то же | то же |
| XA attr обычных файлов | `0x0D55` | `0x0D55` | `0x0811` |

На всех трёх дисках **нет подкаталогов**: корень содержит ровно 6 файлов. Весь контент игры упакован
в `GT2.VOL`.

## 3. Корневое дерево

Колонка "как" = как извлечено (`f1` = cooked Form1, `raw` = `.raw2352`). Скан subheader - полный, по всем секторам.

### USA Simulation v1.2 (SCUS-94488) -> `work\disc_sim_us12`

| LBA | Size (dir record) | Секторов | XA attr | как | Файл | Скан секторов |
|---:|---:|---:|---|---|---|---|
| 23 | 68 | 1 | 0D55 Form1 | f1 | `SYSTEM.CNF` | F1=1 |
| 24 | 628,736 | 307 | 0D55 Form1 | f1 | `SCUS_944.88` | F1=307 |
| 331 | 289,752 | 142 | 0D55 Form1 | f1 | `GT2.OVL` | F1=142 |
| 473 | 488,241,152 | 238,399 | 0D55 Form1 | f1 | `GT2.VOL` | F1=238,399 |
| 238,872 | 85,262,336 | 41,632 | 3D55 Form1+Form2+Interleaved, file no. 1 | raw | `MUSIC.DAT` | F2=41,632; submode 0x64 (+1x 0xE4 EOF); channels 0..21; XA audio coding 0x01 = stereo / 37800 Hz / 4 bit |
| 280,504 | 27,648,000 | 13,500 | 0D55 Form1 | f1 | `FAULTY.PSX` | F1=13,500 |

Записано 614,726,172 байт. Хвост 294,004..294,153 (150 секторов) - пустые (нулевые) сектора, не принадлежащие файлам.

### USA Arcade v1.1 (SCUS-94455) -> `work\disc_arcade_us11`

| LBA | Size (dir record) | Секторов | XA attr | как | Файл | Скан секторов |
|---:|---:|---:|---|---|---|---|
| 23 | 68 | 1 | 0D55 Form1 | f1 | `SYSTEM.CNF` | F1=1 |
| 24 | 628,736 | 307 | 0D55 Form1 | f1 | `SCUS_944.55` | F1=307 |
| 331 | 287,088 | 141 | 0D55 Form1 | f1 | `GT2.OVL` | F1=141 |
| 472 | 213,596,160 | 104,295 | 0D55 Form1 | f1 | `GT2.VOL` | F1=104,295 |
| 104,767 | 85,262,336 | 41,632 | 3D55, file no. 1 | raw | `MUSIC.DAT` | F2=41,632; channels 0..21; XA stereo / 37800 Hz / 4 bit |
| 146,399 | 335,011,840 | 163,580 | 3D55, file no. 1 | raw | `STREAM.DAT` | F1=143,134 (submode 0x48 = Data+RealTime), F2=20,446 (0x64 audio, XA stereo / 37800 Hz / 4 bit); channels 0..7 |

Записано 697,170,676 байт. Хвост 309,979..310,128 (150 секторов) - нулевые.

### JP Disc 1 Arcade (SCPS-10116) -> `work\disc_arcade_jp` (только `SYSTEM.CNF` и `SCPS_101.16`)

| LBA | Size | Секторов | XA attr | Файл |
|---:|---:|---:|---|---|
| 23 | 68 | 1 | 0811 | `SYSTEM.CNF` |
| 24 | 630,784 | 308 | 0811 | `SCPS_101.16` |
| 332 | 285,480 | 140 | 0811 | `GT2.OVL` |
| 472 | 207,036,416 | 101,092 | 0811 | `GT2.VOL` |
| 101,564 | 96,059,392 | 46,904 | 3811, file no. 1 | `MUSIC.DAT` (F2=46,904, channels 0..21) |
| 148,468 | 327,127,040 | 159,730 | 3811, file no. 1 | `STREAM.DAT` (F1=139,765 / F2=19,965, channels 0..7) |

Хвост JP-образа (150 секторов) помечен как Form2 с нулевыми данными (у USA-образов - Form1-нули).

### Карта LBA (одинакова по структуре на всех трёх)

`0..15` system area, `16` PVD, `17` volume descriptor set terminator, `18/19` path table L + копия,
`20/21` path table M + копия, `22` root directory, `23` SYSTEM.CNF, `24` boot EXE, далее файлы вплотную
без дыр и без перекрытий. Данных вне файловой системы (скрытых областей) **не найдено**.

Замечание: размер в directory record для XA-файлов задан в единицах по 2048 байт на сектор
(85,262,336 = 41,632 x 2048), реальный raw-размер `MUSIC.DAT.raw2352` = 97,918,464 байт.

## 4. SYSTEM.CNF

Идентичен по структуре на всех дисках, отличается только имя EXE:

```
BOOT = cdrom:\SCUS_944.88;1      (Simulation US)  | cdrom:\SCUS_944.55;1 (Arcade US) | cdrom:\SCPS_101.16;1 (Arcade JP)
TCB = 4
EVENT = 10
STACK = 801fff00
```

## 5. Заголовки PS-X EXE

| Поле | `SCUS_944.88` (Sim US 1.2) | `SCUS_944.55` (Arcade US 1.1) | `SCPS_101.16` (Arcade JP) |
|---|---|---|---|
| magic | `PS-X EXE` | `PS-X EXE` | `PS-X EXE` |
| pc0 | `0x8005D600` | `0x8005D570` | `0x8005D210` |
| gp0 | `0x00000000` | `0x00000000` | `0x00000000` |
| t_addr | `0x80010000` | `0x80010000` | `0x80010000` |
| t_size | `0x00099000` (626,688) | `0x00099000` (626,688) | `0x00099800` (628,736) |
| конец образа в RAM | `0x800A9000` | `0x800A9000` | `0x800A9800` |
| d_addr / d_size | `0x8009113C` / `0x17C20` | `0x80090E80` / `0x17BD4` | `0x800900A4` / `0x194B0` |
| b_addr / b_size | 0 / 0 | 0 / 0 | 0 / 0 |
| s_addr / s_size | 0 / 0 | 0 / 0 | 0 / 0 |
| слово по offset 0x0C | `0x0008193C` | `0x00081680` | `0x000808A4` |
| region marker (0x4C) | `Sony Computer Entertainment Inc. for North America area` | **`Sony Computer Entertainment Inc. for Japan area`** | `Sony Computer Entertainment Inc. for Japan area` |
| размер файла | 628,736 = 0x800 + t_size | 628,736 = 0x800 + t_size | 630,784 = 0x800 + t_size |
| SHA-1 | `3030aa271c0a4022fc69ce09d76a6bc75e69a32a` | `231f9dba7191b9ef915621662afdc40a7c66df95` | `f4c81a6bc11100b1761e97b9027ff69ac3f8c6b2` |

Проверенные наблюдения:

- `gp0 = 0` и стек берётся из SYSTEM.CNF (`801fff00`), `s_addr = 0`.
- Слово по offset 0x0C во всех трёх EXE в точности равно `d_addr - t_addr + 0x800`, т.е. файловому offset секции data.
- У USA Arcade EXE region marker - "for Japan area" (проверено hex-дампом), хотя диск американский.
- `SCUS_944.55` и `SCUS_944.88` одного размера, но различаются в 395,723 байтах из 628,736 - это разные
  сборки (сдвинутый код), а не патч нескольких байт. Символы/адреса между дисками напрямую не переносятся.

## 6. Arcade vs Simulation (USA): что совпадает

Location-independent SHA-1 (см. раздел 1):

| Файл | Статус | SHA-1 Arcade 1.1 | SHA-1 Simulation 1.2 |
|---|---|---|---|
| `MUSIC.DAT` | **identical** (97,918,464 raw) | `8c9f8fcab73771ed68308a1b7b7e40df29a2bd90` | `8c9f8fcab73771ed68308a1b7b7e40df29a2bd90` |
| `GT2.VOL` | different (213,596,160 vs 488,241,152) | `972c7a65e0cf3c43cce84b219a84da88d98cbdfd` | `8c786a6593fe955bbb4daf61378da87c24bf2169` |
| `GT2.OVL` | different (287,088 vs 289,752) | `e2a4d8e905224d6534c54887d08f0076e32decfa` | `10b85f56a7576647acad371fc75f523672cd3298` |
| `SYSTEM.CNF` | different (только имя EXE) | `e8c39b6b66a8a6de76e9584cd953bde28d1c8d90` | `5430aacb18936011d10e97a8a71315bdf94724d5` |
| `SCUS_944.55` | только Arcade | `231f9dba7191b9ef915621662afdc40a7c66df95` | - |
| `SCUS_944.88` | только Simulation | - | `3030aa271c0a4022fc69ce09d76a6bc75e69a32a` |
| `STREAM.DAT` | только Arcade (384,740,160 raw) | `0ea729da8b186d1e273513419185138cb9bcb65b` | - |
| `FAULTY.PSX` | только Simulation | - | `8971df6ec5aba765699da263a630d68db2527bb8` |

Итог: единственный побайтно общий файл - `MUSIC.DAT`. Общие ассеты внутри `GT2.VOL` надо сравнивать
уже на уровне вложенных файлов (задача следующего scout).

USA Arcade vs JP Arcade: все файлы различаются, включая `MUSIC.DAT` (JP больше: 46,904 против 41,632 секторов) и `STREAM.DAT`.

## 7. Интересные находки и "остатки"

- **Нет** `.SYM`, `.MAP`, readme, debug-файлов или каталогов ни на одном из трёх дисков. Символов нет - reverse engineering EXE пойдёт "с нуля".
- `FAULTY.PSX` (только Simulation, 27,648,000 байт = 13,500 секторов) - **целиком нули** (проверено полным проходом). Это dummy/padding в конце диска. **ДОГАДКА**: служит для выноса данных от внешнего края диска / как заглушка; в игре как ассет не используется.
- В boot EXE лежит открытая таблица путей содержимого `GT2.VOL`: 231 уникальная path-like строка в Simulation EXE, 214 в Arcade US, 124 в JP. Верхние каталоги (Sim): `dirt`=81, `arcade`=58, `carparam`=36, `gtmenu`=30, `sound`=14, `replay`=6, `license`=3, `engine`=2, `font`=1. Типы: `.tim`, `.tim.gz`, `.dat`, `.dat.gz`. Также в EXE есть имена `gt2.ovl`, `gt2.vol`, `music.dat`, `stream.dat`, `notice.tim`, `logo-scea.tim` (JP: `logo-scei.tim`, `logo-pdi.tim`).
- Dev-остаток: во всех трёх EXE есть строка `/carparam/gtmode_data_dev.dat.gz`. Существует ли сам файл в `GT2.VOL` - не проверялось.
- Simulation EXE ссылается на `stream.dat`, хотя на диске Simulation этого файла нет.
- `GT2.OVL` (проверено распаковкой в `work\ovl\<disc>\ovlN.bin`): в начале таблица из 6 пар `u32 offset, u32 packed_size` (первый offset = 0x30), далее 6 gzip-потоков (`1F 8B 08`). Каждый распаковывается в сырой MIPS-код (первые слова вида `27BDFFE0` = `addiu sp,sp,-0x20`). Размеры после распаковки, Simulation: 316,920 / 248,004 / 275,780 / 11,500 / 273,012 / 8,416; Arcade US: 316,776 / 245,144 / 275,312 / 11,500 / 272,888 / 8,416. Адрес загрузки overlay и соответствие "overlay -> режим игры" не определялись.
- `GT2.VOL`: magic `GTFS`, `u32@0x08` = `0x2D642D3D` (Sim) / `0x2994297D` (Arcade), `u16@0x10` = 764 (Sim) / 508 (Arcade), с 0x14 идёт монотонно возрастающая таблица `u32`. Проверено на первых трёх значениях: `value >> 11` = номер 2048-байтного блока внутри VOL (0xAD80 -> 0xA800, 0x5E322 -> 0x5E000, 0x613FE -> 0x61000). По первому offset лежат 32-байтные записи: `u32` timestamp (0x384E83CF, начало декабря 1999), `u16` индекс, байт, далее имя с NUL-padding (`.carcolor`, `.carinfoa`, `.carinfoe`, ...). По второму/третьему - данные с сигнатурами `CCOL00`, `CAR\0`. **ДОГАДКА**: младшие 11 бит значения = число неиспользованных байт в последнем блоке; 508/764 - число записей верхнего уровня, а не всех файлов (последнее значение таблицы указывает всего на ~9 MB / ~12 MB при размере VOL 213 MB / 488 MB). Полный разбор формата - отдельная задача.
- `MUSIC.DAT`: чистый XA ADPCM, только Form2, 22 канала (0..21), file number 1, coding info 0x01 (stereo, 37800 Hz, 4 bit). Cooked-извлечение его бы испортило - поэтому raw.
- `STREAM.DAT` (только Arcade-диски): interleaved. Data-сектора (submode 0x48) на канале 0 имеют заголовок `0x0160` + слово типа `0x5349`, а не стандартное для PS1 STR `0x8001`; на каналах 1..7 в первых 4000 секторах data-сектора с нулевым заголовком (1,393 шт.). Т.е. FMV - **нестандартный вариант STR**; стандартные STR-декодеры могут не подойти. Формат кадра не разбирался.
- Даты в directory records USA-дисков - 1999-10-12 (у `STREAM.DAT` - 1999-10-26), при этом PVD creation date - 1999-12-10, а timestamp внутри `GT2.VOL` - декабрь 1999. Т.е. даты directory records не отражают реальную дату сборки.

## 8. Ограничения и что не делалось

- EDC/ECC секторов не проверялись (целостность образов подтверждается косвенно: PVD volume size совпадает с размером образа, все структуры читаются, gzip-потоки `GT2.OVL` распаковываются без ошибок).
- Сверка образов с Redump-хэшами не делалась (нет сети/базы в рамках задачи).
- Для JP-диска извлечены только `SYSTEM.CNF` и EXE (по заданию); листинг, карта и сравнение - полные.
- Subchannel/LibCrypt не применимо: образы `.bin` без `.sub`; USA/JP релизы GT2 LibCrypt не используют (**из памяти, low confidence**, на данных не проверялось).
- В корне проекта ещё нет `.gitignore`; `work\` (около 1.3 GB) и `bin\`/`obj\` нужно исключить до первого commit.
